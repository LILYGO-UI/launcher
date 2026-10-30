#include "bluez_agent.h"

#include "ble_framing.h"
#include "storage_ipc.h"
#include "webrtc_session.h"

#include <gio/gio.h>
#include <glib-unix.h>

#include <array>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef __linux__
#include <fcntl.h>
#include <linux/rfkill.h>
#include <unistd.h>
#endif

namespace launcher::transport {
namespace {

constexpr const char *kRoot = "/cc/lilygo/ui/Launcher/share";
constexpr const char *kService = "/cc/lilygo/ui/Launcher/share/service";
constexpr const char *kReceive = "/cc/lilygo/ui/Launcher/share/service/rx";
constexpr const char *kTransmit = "/cc/lilygo/ui/Launcher/share/service/tx";
constexpr const char *kAdvertisement = "/cc/lilygo/ui/Launcher/share_advertisement";
constexpr const char *kObjectManager = "org.freedesktop.DBus.ObjectManager";
constexpr const char *kProperties = "org.freedesktop.DBus.Properties";
constexpr const char *kGattService = "org.bluez.GattService1";
constexpr const char *kCharacteristic = "org.bluez.GattCharacteristic1";
constexpr const char *kAdvertisementInterface = "org.bluez.LEAdvertisement1";
constexpr const char *kIntrospection = R"xml(
<node>
  <interface name="org.freedesktop.DBus.ObjectManager">
    <method name="GetManagedObjects"><arg type="a{oa{sa{sv}}}" direction="out"/></method>
  </interface>
  <interface name="org.bluez.GattService1">
    <property name="UUID" type="s" access="read"/>
    <property name="Primary" type="b" access="read"/>
    <property name="Includes" type="ao" access="read"/>
  </interface>
  <interface name="org.bluez.GattCharacteristic1">
    <method name="ReadValue"><arg type="a{sv}" direction="in"/><arg type="ay" direction="out"/></method>
    <method name="StartNotify"/>
    <method name="StopNotify"/>
    <property name="UUID" type="s" access="read"/>
    <property name="Service" type="o" access="read"/>
    <property name="Flags" type="as" access="read"/>
    <property name="Notifying" type="b" access="read"/>
    <property name="Value" type="ay" access="read"/>
  </interface>
  <interface name="org.bluez.LEAdvertisement1">
    <method name="Release"/>
    <property name="Type" type="s" access="read"/>
    <property name="ServiceUUIDs" type="as" access="read"/>
    <property name="LocalName" type="s" access="read"/>
    <property name="Discoverable" type="b" access="read"/>
  </interface>
</node>)xml";
constexpr const char *kReceiveIntrospection = R"xml(
<node>
  <interface name="org.bluez.GattCharacteristic1">
    <method name="WriteValue"><arg type="ay" direction="in"/><arg type="a{sv}" direction="in"/></method>
    <property name="UUID" type="s" access="read"/>
    <property name="Service" type="o" access="read"/>
    <property name="Flags" type="as" access="read"/>
  </interface>
</node>)xml";

GVariant *empty_options() {
    return g_variant_new_array(G_VARIANT_TYPE("{sv}"), nullptr, 0);
}

GVariant *byte_array(const std::vector<std::uint8_t> &value) {
    return g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, value.data(), value.size(), 1);
}

void remove_source(guint &source) {
    if (source != 0) {
        g_source_remove(source);
        source = 0;
    }
}

void unblock_bluetooth() {
#ifdef __linux__
    const int descriptor = open("/dev/rfkill", O_WRONLY | O_CLOEXEC | O_NONBLOCK);
    if (descriptor >= 0) {
        rfkill_event event{};
        event.type = RFKILL_TYPE_BLUETOOTH;
        event.op = RFKILL_OP_CHANGE_ALL;
        const auto ignored = write(descriptor, &event, sizeof(event));
        static_cast<void>(ignored);
        close(descriptor);
    }
#endif
}

std::string advertisement_name_for_address(const char *address)
{
    std::string hex;
    if (address != nullptr) {
        for (const unsigned char character : std::string(address)) {
            if (std::isxdigit(character)) {
                hex.push_back(static_cast<char>(std::tolower(character)));
            }
        }
    }
    if (hex.size() < 4) {
        hex = "0000";
    } else {
        hex = hex.substr(hex.size() - 4);
    }
    return std::string(kAdvertisementNamePrefix) + hex;
}

}

struct BluezAgent::Impl : std::enable_shared_from_this<BluezAgent::Impl> {
    using Reply = std::function<void(Impl &, GVariant *, GError *)>;

    struct PendingCall {
        std::weak_ptr<Impl> owner;
        Reply reply;
    };

    struct Outgoing {
        std::vector<std::vector<std::uint8_t>> frames;
        std::size_t next = 0;
    };

    Storage &storage;
    std::unique_ptr<Session> session;
    FrameDecoder decoder;
    GMainContext *context = nullptr;
    GMainLoop *loop = nullptr;
    GDBusConnection *bus = nullptr;
    GCancellable *startup = nullptr;
    GDBusNodeInfo *introspection = nullptr;
    GDBusNodeInfo *receive_introspection = nullptr;
    std::string bluez_owner;
    std::string adapter;
    std::string advertisement_name = std::string(kAdvertisementNamePrefix) + "0000";
    std::string device;
    std::string failure;
    std::vector<guint> registrations;
    std::deque<nlohmann::json> incoming;
    std::deque<Outgoing> outgoing;
    std::vector<std::uint8_t> value;
    std::array<guint, 2> signal_sources{};
    guint owner_subscription = 0;
    guint device_subscription = 0;
    gulong bus_closed_handler = 0;
    guint startup_timeout = 0;
    guint cleanup_timeout = 0;
    guint poll_source = 0;
    guint send_source = 0;
    guint receive_source = 0;
    guint notify_timeout = 0;
    guint release_timeout = 0;
    unsigned cleanup_pending = 0;
    bool notifying = false;
    bool registered_gatt = false;
    bool registered_advertisement = false;
    bool stopping = false;

    explicit Impl(Storage &storage_value)
        : storage(storage_value),
          context(g_main_context_ref_thread_default()),
          loop(g_main_loop_new(context, false)), startup(g_cancellable_new()) {}

    ~Impl() {
        remove_source(startup_timeout);
        remove_source(cleanup_timeout);
        remove_source(poll_source);
        remove_source(send_source);
        remove_source(receive_source);
        remove_source(notify_timeout);
        remove_source(release_timeout);
        for (auto &source : signal_sources) {
            remove_source(source);
        }
        session.reset();
        if (bus != nullptr) {
            if (owner_subscription != 0) {
                g_dbus_connection_signal_unsubscribe(bus, owner_subscription);
            }
            if (device_subscription != 0) {
                g_dbus_connection_signal_unsubscribe(bus, device_subscription);
            }
            if (bus_closed_handler != 0) {
                g_signal_handler_disconnect(bus, bus_closed_handler);
            }
            for (const guint registration : registrations) {
                g_dbus_connection_unregister_object(bus, registration);
            }
            g_object_unref(bus);
        }
        if (introspection != nullptr) {
            g_dbus_node_info_unref(introspection);
        }
        if (receive_introspection != nullptr) {
            g_dbus_node_info_unref(receive_introspection);
        }
        g_object_unref(startup);
        g_main_loop_unref(loop);
        g_main_context_unref(context);
    }

    void initialize() {
        const auto weak = weak_from_this();
        session = std::make_unique<Session>(storage, [weak](const nlohmann::json &envelope) {
            if (const auto owner = weak.lock()) {
                owner->send(envelope);
            }
        }, [main_context = context](std::function<void()> operation) {
            auto *pending = new std::function<void()>(std::move(operation));
            GSource *source = g_idle_source_new();
            g_source_set_priority(source, G_PRIORITY_DEFAULT);
            g_source_set_callback(source, [](gpointer data) -> gboolean {
                try {
                    (*static_cast<std::function<void()> *>(data))();
                } catch (const std::exception &error) {
                    std::fprintf(stderr, "File sharing callback failed: %s\n", error.what());
                }
                return G_SOURCE_REMOVE;
            }, pending, [](gpointer data) { delete static_cast<std::function<void()> *>(data); });
            g_source_attach(source, main_context);
            g_source_unref(source);
        });
        for (std::size_t index = 0; index < signal_sources.size(); ++index) {
            signal_sources[index] = g_unix_signal_add(index == 0 ? SIGINT : SIGTERM, [](gpointer data) -> gboolean {
                static_cast<Impl *>(data)->stop();
                return G_SOURCE_CONTINUE;
            }, this);
        }
        startup_timeout = g_timeout_add(20000, [](gpointer data) -> gboolean {
            auto &owner = *static_cast<Impl *>(data);
            owner.startup_timeout = 0;
            owner.fail("Bluetooth registration timed out");
            return G_SOURCE_REMOVE;
        }, this);
        poll_source = g_timeout_add(50, [](gpointer data) -> gboolean {
            auto &owner = *static_cast<Impl *>(data);
            try {
                owner.session->tick();
                if (owner.session->connected()) {
                    remove_source(owner.release_timeout);
                    owner.retire_advertisement();
                }
                if (owner.session->closed() && owner.outgoing.empty()) {
                    owner.poll_source = 0;
                    owner.stop();
                    return G_SOURCE_REMOVE;
                }
            } catch (const std::exception &error) {
                owner.poll_source = 0;
                owner.fail(error.what());
                return G_SOURCE_REMOVE;
            }
            return G_SOURCE_CONTINUE;
        }, this);
        unblock_bluetooth();
        auto *pending = new std::weak_ptr<Impl>(weak);
        g_bus_get(G_BUS_TYPE_SYSTEM, startup, [](GObject *, GAsyncResult *result, gpointer data) {
            const std::unique_ptr<std::weak_ptr<Impl>> weak_owner(static_cast<std::weak_ptr<Impl> *>(data));
            GError *error = nullptr;
            GDBusConnection *connection = g_bus_get_finish(result, &error);
            if (const auto owner = weak_owner->lock()) {
                if (!owner->stopping) {
                    if (connection == nullptr) {
                        owner->fail(error != nullptr ? error->message : "Cannot connect to the system bus");
                    } else {
                        owner->bus = connection;
                        connection = nullptr;
                        owner->connected_bus();
                    }
                }
            }
            g_clear_object(&connection);
            g_clear_error(&error);
        }, pending);
    }

    void call(const char *destination, const char *path, const char *interface, const char *method,
              GVariant *parameters, const GVariantType *reply_type, Reply reply, bool cleaning = false) {
        auto *pending = new PendingCall{weak_from_this(), std::move(reply)};
        g_dbus_connection_call(bus, destination, path, interface, method, parameters, reply_type,
            G_DBUS_CALL_FLAGS_NONE, cleaning ? 2000 : 20000, cleaning ? nullptr : startup,
            [](GObject *source, GAsyncResult *result, gpointer data) {
                const std::unique_ptr<PendingCall> pending_call(static_cast<PendingCall *>(data));
                GError *error = nullptr;
                GVariant *response = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
                if (const auto owner = pending_call->owner.lock()) {
                    try {
                        pending_call->reply(*owner, response, error);
                    } catch (const std::exception &exception) {
                        owner->fail(exception.what());
                    }
                }
                g_clear_pointer(&response, g_variant_unref);
                g_clear_error(&error);
            }, pending);
    }

    static void check_reply(GVariant *response, GError *error) {
        if (response == nullptr) {
            throw std::runtime_error(error != nullptr ? error->message : "The Bluetooth service did not reply");
        }
    }

    void connected_bus() {
        g_dbus_connection_set_exit_on_close(bus, false);
        bus_closed_handler = g_signal_connect(bus, "closed", G_CALLBACK(+[](GDBusConnection *, gboolean, GError *, gpointer data) {
            auto &owner = *static_cast<Impl *>(data);
            owner.registered_gatt = false;
            owner.registered_advertisement = false;
            owner.bluez_owner.clear();
            if (!owner.session->connected()) {
                owner.fail("The system bus disconnected");
            }
        }), this);
        owner_subscription = g_dbus_connection_signal_subscribe(bus, "org.freedesktop.DBus",
            "org.freedesktop.DBus", "NameOwnerChanged", "/org/freedesktop/DBus", "org.bluez",
            G_DBUS_SIGNAL_FLAGS_NONE, [](GDBusConnection *, const gchar *, const gchar *, const gchar *,
                const gchar *, GVariant *parameters, gpointer data) {
                auto &owner = *static_cast<Impl *>(data);
                const gchar *name = nullptr;
                const gchar *old_owner = nullptr;
                const gchar *new_owner = nullptr;
                g_variant_get(parameters, "(&s&s&s)", &name, &old_owner, &new_owner);
                if (!owner.bluez_owner.empty() && owner.bluez_owner == old_owner) {
                    owner.bluez_owner.clear();
                    owner.registered_gatt = false;
                    owner.registered_advertisement = false;
                    owner.bluetooth_disconnected();
                }
            }, this, nullptr);
        call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetNameOwner",
            g_variant_new("(s)", "org.bluez"), G_VARIANT_TYPE("(s)"), [](Impl &owner, GVariant *response, GError *error) {
                if (owner.stopping) { return; }
                check_reply(response, error);
                const gchar *name = nullptr;
                g_variant_get(response, "(&s)", &name);
                owner.bluez_owner = name;
                owner.monitor_device();
                owner.find_adapter();
            });
    }

    void monitor_device() {
        device_subscription = g_dbus_connection_signal_subscribe(bus, bluez_owner.c_str(), kProperties,
            "PropertiesChanged", nullptr, "org.bluez.Device1", G_DBUS_SIGNAL_FLAGS_NONE,
            [](GDBusConnection *, const gchar *sender, const gchar *path, const gchar *, const gchar *,
                GVariant *parameters, gpointer data) {
                auto &owner = *static_cast<Impl *>(data);
                if (owner.bluez_owner != sender) { return; }
                const gchar *interface = nullptr;
                GVariant *changed = nullptr;
                GVariant *invalidated = nullptr;
                g_variant_get(parameters, "(&s@a{sv}@as)", &interface, &changed, &invalidated);
                gboolean connected = true;
                const bool has_connection_change = g_variant_lookup(changed, "Connected", "b", &connected);
                /* BlueZ reports Device1 before the first GATT WriteValue. Bind
                 * only after notifications are enabled so an unrelated
                 * Bluetooth keyboard/audio device cannot claim the session. */
                if (owner.device.empty() && owner.notifying && has_connection_change && connected) {
                    owner.device = path;
                }
                if (owner.device == path && has_connection_change && !connected) {
                    owner.bluetooth_disconnected();
                }
                g_variant_unref(changed);
                g_variant_unref(invalidated);
            }, this, nullptr);
    }

    void find_adapter() {
        call(bluez_owner.c_str(), "/", kObjectManager, "GetManagedObjects", nullptr,
            G_VARIANT_TYPE("(a{oa{sa{sv}}})"), [](Impl &owner, GVariant *response, GError *error) {
                if (owner.stopping) { return; }
                check_reply(response, error);
                GVariant *objects = g_variant_get_child_value(response, 0);
                GVariantIter iterator;
                g_variant_iter_init(&iterator, objects);
                const gchar *path = nullptr;
                GVariant *interfaces = nullptr;
                while (g_variant_iter_next(&iterator, "{&o@a{sa{sv}}}", &path, &interfaces)) {
                    GVariant *gatt = g_variant_lookup_value(interfaces, "org.bluez.GattManager1", G_VARIANT_TYPE("a{sv}"));
                    GVariant *advertising = g_variant_lookup_value(interfaces, "org.bluez.LEAdvertisingManager1", G_VARIANT_TYPE("a{sv}"));
                    if (gatt != nullptr && advertising != nullptr) {
                        owner.adapter = path;
                        GVariant *adapter_properties = g_variant_lookup_value(
                            interfaces, "org.bluez.Adapter1", G_VARIANT_TYPE("a{sv}"));
                        const gchar *address = nullptr;
                        if (adapter_properties != nullptr &&
                            g_variant_lookup(adapter_properties, "Address", "&s", &address)) {
                            owner.advertisement_name = advertisement_name_for_address(address);
                        }
                        g_clear_pointer(&adapter_properties, g_variant_unref);
                    }
                    g_clear_pointer(&gatt, g_variant_unref);
                    g_clear_pointer(&advertising, g_variant_unref);
                    g_variant_unref(interfaces);
                    if (!owner.adapter.empty()) { break; }
                }
                g_variant_unref(objects);
                if (owner.adapter.empty()) {
                    throw std::runtime_error("No Bluetooth adapter supports GATT peripheral sharing");
                }
                owner.power_adapter();
            });
    }

    void power_adapter() {
        call(bluez_owner.c_str(), adapter.c_str(), kProperties, "Set",
            g_variant_new("(ssv)", "org.bluez.Adapter1", "Powered", g_variant_new_boolean(true)),
            G_VARIANT_TYPE_UNIT, [](Impl &owner, GVariant *response, GError *error) {
                if (owner.stopping) { return; }
                check_reply(response, error);
                owner.set_adapter_alias();
            });
    }

    void set_adapter_alias() {
        /* BlueZ also publishes registered GATT UUIDs through the adapter's
         * classic EIR. Keep that name aligned with the LE advertisement so a
         * browser cannot select the controller's old global name and then
         * time out while trying to open GATT. */
        call(bluez_owner.c_str(), adapter.c_str(), kProperties, "Set",
            g_variant_new("(ssv)", "org.bluez.Adapter1", "Alias",
                          g_variant_new_string(advertisement_name.c_str())),
            G_VARIANT_TYPE_UNIT, [](Impl &owner, GVariant *response, GError *error) {
                if (owner.stopping) { return; }
                check_reply(response, error);
                owner.export_objects();
                owner.register_application();
            });
    }

    GVariant *property(const char *path, const char *interface, const char *name) {
        const std::string property_name(name);
        if (std::string(interface) == kGattService) {
            if (property_name == "UUID") { return g_variant_new_string(kServiceUuid); }
            if (property_name == "Primary") { return g_variant_new_boolean(true); }
            if (property_name == "Includes") { return g_variant_new_objv(nullptr, 0); }
        } else if (std::string(interface) == kCharacteristic) {
            const bool transmit = std::string(path) == kTransmit;
            if (property_name == "UUID") { return g_variant_new_string(transmit ? kTxUuid : kRxUuid); }
            if (property_name == "Service") { return g_variant_new_object_path(kService); }
            if (property_name == "Flags") {
                if (transmit) {
                    const char *flags[] = {"read", "notify"};
                    return g_variant_new_strv(flags, 2);
                }
                const char *flags[] = {"write", "write-without-response"};
                return g_variant_new_strv(flags, 2);
            }
            if (transmit && property_name == "Notifying") { return g_variant_new_boolean(notifying); }
            if (transmit && property_name == "Value") { return byte_array(value); }
        } else if (std::string(interface) == kAdvertisementInterface) {
            if (property_name == "Type") { return g_variant_new_string("peripheral"); }
            if (property_name == "ServiceUUIDs") {
                const char *uuid = kServiceUuid;
                return g_variant_new_strv(&uuid, 1);
            }
            if (property_name == "LocalName") { return g_variant_new_string(advertisement_name.c_str()); }
            if (property_name == "Discoverable") { return g_variant_new_boolean(true); }
        }
        return nullptr;
    }

    GVariant *properties(const char *path, const char *interface) {
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
        GDBusInterfaceInfo *info = g_dbus_node_info_lookup_interface(
            std::string(path) == kReceive ? receive_introspection : introspection, interface);
        for (GDBusPropertyInfo **entry = info->properties; entry != nullptr && *entry != nullptr; ++entry) {
            if (GVariant *property_value = property(path, interface, (*entry)->name)) {
                g_variant_builder_add(&builder, "{sv}", (*entry)->name, property_value);
            }
        }
        return g_variant_builder_end(&builder);
    }

    GVariant *managed_objects() {
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("a{oa{sa{sv}}}"));
        for (const char *path : {kService, kReceive, kTransmit}) {
            const char *interface = std::string(path) == kService ? kGattService : kCharacteristic;
            GVariantBuilder interfaces;
            g_variant_builder_init(&interfaces, G_VARIANT_TYPE("a{sa{sv}}"));
            g_variant_builder_add(&interfaces, "{s@a{sv}}", interface, properties(path, interface));
            g_variant_builder_add(&builder, "{o@a{sa{sv}}}", path, g_variant_builder_end(&interfaces));
        }
        return g_variant_new("(@a{oa{sa{sv}}})", g_variant_builder_end(&builder));
    }

    void export_objects() {
        GError *error = nullptr;
        introspection = g_dbus_node_info_new_for_xml(kIntrospection, &error);
        if (introspection == nullptr) {
            const std::string message = error != nullptr ? error->message : "Cannot describe the Bluetooth service";
            g_clear_error(&error);
            throw std::runtime_error(message);
        }
        receive_introspection = g_dbus_node_info_new_for_xml(kReceiveIntrospection, &error);
        if (receive_introspection == nullptr) {
            const std::string message = error != nullptr ? error->message : "Cannot describe the Bluetooth receive service";
            g_clear_error(&error);
            throw std::runtime_error(message);
        }
        static const GDBusInterfaceVTable vtable{
            [](GDBusConnection *, const gchar *sender, const gchar *path, const gchar *interface,
                const gchar *method, GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data) {
                auto &owner = *static_cast<Impl *>(data);
                if ((std::string(interface) == kCharacteristic || std::string(interface) == kAdvertisementInterface)
                    && (owner.bluez_owner.empty() || owner.bluez_owner != sender)) {
                    g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.NotAuthorized",
                        "Only BlueZ may call the sharing GATT service");
                    return;
                }
                try {
                    owner.method(path, interface, method, parameters, invocation);
                } catch (const std::exception &exception) {
                    std::fprintf(stderr, "File sharing D-Bus operation %s %s.%s failed: %s\n",
                        path, interface, method, exception.what());
                    g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Failed", exception.what());
                }
            },
            [](GDBusConnection *, const gchar *, const gchar *path, const gchar *interface,
                const gchar *name, GError **error_output, gpointer data) -> GVariant * {
                auto &owner = *static_cast<Impl *>(data);
                GVariant *result = owner.property(path, interface, name);
                if (result == nullptr) {
                    g_set_error(error_output, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "Unknown property %s", name);
                }
                return result;
            },
            nullptr, {}
        };
        const std::array<std::pair<const char *, const char *>, 5> objects{{
            {kRoot, kObjectManager}, {kService, kGattService}, {kReceive, kCharacteristic},
            {kTransmit, kCharacteristic}, {kAdvertisement, kAdvertisementInterface}
        }};
        for (const auto &[path, interface] : objects) {
            const guint registration = g_dbus_connection_register_object(bus, path,
                g_dbus_node_info_lookup_interface(std::string(path) == kReceive ? receive_introspection : introspection, interface),
                &vtable, this, nullptr, &error);
            if (registration == 0) {
                const std::string message = error != nullptr ? error->message : "Cannot export the Bluetooth service";
                g_clear_error(&error);
                throw std::runtime_error(message);
            }
            registrations.push_back(registration);
        }
    }

    void register_application() {
        call(bluez_owner.c_str(), adapter.c_str(), "org.bluez.GattManager1", "RegisterApplication",
            g_variant_new("(o@a{sv})", kRoot, empty_options()), G_VARIANT_TYPE_UNIT,
            [](Impl &owner, GVariant *response, GError *error) {
                if (owner.stopping) { return; }
                check_reply(response, error);
                owner.registered_gatt = true;
                owner.register_advertisement();
            });
    }

    void register_advertisement() {
        call(bluez_owner.c_str(), adapter.c_str(), "org.bluez.LEAdvertisingManager1", "RegisterAdvertisement",
            g_variant_new("(o@a{sv})", kAdvertisement, empty_options()), G_VARIANT_TYPE_UNIT,
            [](Impl &owner, GVariant *response, GError *error) {
                if (owner.stopping) { return; }
                check_reply(response, error);
                owner.registered_advertisement = true;
                remove_source(owner.startup_timeout);
                const std::string status = "Connect to " + owner.advertisement_name +
                    " from the browser on the same network.";
                owner.storage.request('S', {status.begin(), status.end()});
            });
    }

    void emit_property(const char *name, GVariant *new_value) {
        GVariantBuilder changed;
        g_variant_builder_init(&changed, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&changed, "{sv}", name, new_value);
        GError *error = nullptr;
        if (!g_dbus_connection_emit_signal(bus, nullptr, kTransmit, kProperties, "PropertiesChanged",
                g_variant_new("(s@a{sv}@as)", kCharacteristic, g_variant_builder_end(&changed),
                    g_variant_new_strv(nullptr, 0)), &error)) {
            const std::string message = error != nullptr ? error->message : "Cannot send Bluetooth signaling";
            g_clear_error(&error);
            throw std::runtime_error(message);
        }
    }

    void method(const char *path, const char *interface, const char *name, GVariant *parameters,
                GDBusMethodInvocation *invocation) {
        const std::string method_name(name);
        if (std::string(interface) == kObjectManager && method_name == "GetManagedObjects") {
            g_dbus_method_invocation_return_value(invocation, managed_objects());
            return;
        }
        if (std::string(interface) == kAdvertisementInterface && method_name == "Release") {
            g_dbus_method_invocation_return_value(invocation, nullptr);
            if (registered_advertisement) {
                registered_advertisement = false;
                stop();
            }
            return;
        }
        if (stopping) {
            g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.NotPermitted", "File sharing is closing");
            return;
        }
        if (std::string(path) == kReceive && method_name == "WriteValue") {
            static unsigned trace_writes = 0;
            const bool trace_gatt = g_getenv("LILYGO_TRACE_GATT") != nullptr;
            const unsigned trace_index = trace_gatt && trace_writes < 64 ? ++trace_writes : 0;
            if (trace_index != 0) {
                GVariant *trace_bytes = nullptr;
                GVariant *trace_options = nullptr;
                g_variant_get(parameters, "(@ay@a{sv})", &trace_bytes, &trace_options);
                const std::unique_ptr<GVariant, decltype(&g_variant_unref)> bytes_owner(trace_bytes, g_variant_unref);
                const std::unique_ptr<GVariant, decltype(&g_variant_unref)> options_owner(trace_options, g_variant_unref);
                const gchar *trace_device = nullptr;
                const gchar *trace_type = nullptr;
                g_variant_lookup(trace_options, "device", "&o", &trace_device);
                g_variant_lookup(trace_options, "type", "&s", &trace_type);
                gsize trace_size = 0;
                const auto *trace_value = static_cast<const std::uint8_t *>(
                    g_variant_get_fixed_array(trace_bytes, &trace_size, 1));
                const unsigned trace_sequence = trace_size >= 4
                    ? (static_cast<unsigned>(trace_value[2]) << 8) | trace_value[3] : 0;
                std::fprintf(stderr, "GATT WriteValue #%u device=%s type=%s bytes=%zu seq=%u notifying=%s\n",
                    trace_writes, trace_device ? trace_device : "<absent>", trace_type ? trace_type : "<absent>",
                    trace_size, trace_sequence, notifying ? "yes" : "no");
            }
            receive(parameters, invocation);
            if (trace_index != 0) {
                std::fprintf(stderr, "GATT WriteValue #%u handler returned\n", trace_index);
            }
            return;
        }
        if (std::string(path) == kTransmit && method_name == "ReadValue") {
            GVariant *options = nullptr;
            g_variant_get(parameters, "(@a{sv})", &options);
            const std::unique_ptr<GVariant, decltype(&g_variant_unref)> options_owner(options, g_variant_unref);
            guint16 offset = 0;
            g_variant_lookup(options, "offset", "q", &offset);
            if (offset > value.size()) {
                g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.InvalidOffset",
                    "Read offset is outside the notification value");
                return;
            }
            const std::vector<std::uint8_t> result(value.begin() + offset, value.end());
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(@ay)", byte_array(result)));
            return;
        }
        if (std::string(path) == kTransmit && method_name == "StartNotify") {
            if (!notifying) {
                notifying = true;
                emit_property("Notifying", g_variant_new_boolean(true));
                remove_source(notify_timeout);
                notify_timeout = g_timeout_add(15000, [](gpointer data) -> gboolean {
                    auto &owner = *static_cast<Impl *>(data);
                    owner.notify_timeout = 0;
                    if (!owner.session->connected() && !owner.session->channel_pending()) {
                        owner.stop();
                    }
                    return G_SOURCE_REMOVE;
                }, this);
            }
            g_dbus_method_invocation_return_value(invocation, nullptr);
            return;
        }
        if (std::string(path) == kTransmit && method_name == "StopNotify") {
            notifying = false;
            remove_source(notify_timeout);
            emit_property("Notifying", g_variant_new_boolean(false));
            g_dbus_method_invocation_return_value(invocation, nullptr);
            bluetooth_disconnected();
            return;
        }
        g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.NotSupported", "Unsupported GATT operation");
    }

    void receive(GVariant *parameters, GDBusMethodInvocation *invocation) {
        GVariant *bytes = nullptr;
        GVariant *options = nullptr;
        g_variant_get(parameters, "(@ay@a{sv})", &bytes, &options);
        const std::unique_ptr<GVariant, decltype(&g_variant_unref)> bytes_owner(bytes, g_variant_unref);
        const std::unique_ptr<GVariant, decltype(&g_variant_unref)> options_owner(options, g_variant_unref);
        guint16 offset = 0;
        g_variant_lookup(options, "offset", "q", &offset);
        if (offset != 0) {
            std::fprintf(stderr, "GATT WriteValue rejected: unsupported offset %u\n", offset);
            g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.InvalidOffset", "Long writes are not supported");
            return;
        }
        const gchar *sender_device = nullptr;
        if (!g_variant_lookup(options, "device", "&o", &sender_device) || !notifying) {
            std::fprintf(stderr, "GATT WriteValue rejected: device=%s notifying=%s\n",
                sender_device ? sender_device : "<absent>", notifying ? "yes" : "no");
            g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.NotPermitted", "Subscribe before sending signaling");
            return;
        }
        if (!device.empty() && device != sender_device) {
            std::fprintf(stderr, "GATT WriteValue rejected: another device is already bound\n");
            g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.NotAuthorized", "Sharing already has a browser");
            return;
        }
        gsize size = 0;
        const auto *data = static_cast<const std::uint8_t *>(g_variant_get_fixed_array(bytes, &size, 1));
        if (size < 5 || size > 20) {
            decoder.reset();
            throw std::runtime_error("Invalid Bluetooth frame length");
        }
        /* Bind as soon as the first valid frame arrives.  A browser may be
         * interrupted while sending a multi-frame envelope; keeping the
         * sender identity across that partial message prevents a second
         * connection from interleaving frames during a restart/reconnect. */
        device = sender_device;
        remove_source(notify_timeout);
        auto envelope = decoder.feed({data, data + size});
        if (envelope.has_value()) {
            if (incoming.size() >= 8) {
                throw std::runtime_error("Bluetooth signaling queue is full");
            }
            incoming.push_back(std::move(*envelope));
            if (receive_source == 0) {
                receive_source = g_idle_add([](gpointer data_value) -> gboolean {
                    auto &owner = *static_cast<Impl *>(data_value);
                    if (owner.incoming.empty() || owner.stopping) {
                        owner.receive_source = 0;
                        return G_SOURCE_REMOVE;
                    }
                    auto message = std::move(owner.incoming.front());
                    owner.incoming.pop_front();
                    try {
                        owner.session->handle_signal(message);
                    } catch (const std::exception &error) {
                        owner.receive_source = 0;
                        owner.fail(error.what());
                        return G_SOURCE_REMOVE;
                    }
                    return G_SOURCE_CONTINUE;
                }, this);
            }
        }
        g_dbus_method_invocation_return_value(invocation, nullptr);
    }

    void send(const nlohmann::json &envelope) {
        if (!notifying || stopping || bluez_owner.empty() || bus == nullptr || g_dbus_connection_is_closed(bus)) {
            throw std::runtime_error("Bluetooth signaling disconnected");
        }
        if (outgoing.size() >= 8) {
            throw std::runtime_error("Bluetooth notification queue is full");
        }
        outgoing.push_back(Outgoing{encode_message(envelope), 0});
        if (send_source == 0) {
            send_source = g_timeout_add(10, [](gpointer data) -> gboolean {
                auto &owner = *static_cast<Impl *>(data);
                if (!owner.notifying || owner.stopping) {
                    owner.outgoing.clear();
                    owner.send_source = 0;
                    return G_SOURCE_REMOVE;
                }
                try {
                    auto &message = owner.outgoing.front();
                    owner.value = std::move(message.frames[message.next++]);
                    owner.emit_property("Value", byte_array(owner.value));
                    if (message.next == message.frames.size()) { owner.outgoing.pop_front(); }
                } catch (const std::exception &error) {
                    owner.send_source = 0;
                    owner.outgoing.clear();
                    if (!owner.session->connected()) { owner.fail(error.what()); }
                    return G_SOURCE_REMOVE;
                }
                if (owner.outgoing.empty()) {
                    owner.send_source = 0;
                    return G_SOURCE_REMOVE;
                }
                return G_SOURCE_CONTINUE;
            }, this);
        }
    }

    void bluetooth_disconnected() {
        if (stopping || session->connected()) { return; }
        if (!session->channel_pending()) {
            stop();
            return;
        }
        if (release_timeout == 0) {
            release_timeout = g_timeout_add(5000, [](gpointer data) -> gboolean {
                auto &owner = *static_cast<Impl *>(data);
                owner.release_timeout = 0;
                if (!owner.session->connected()) { owner.stop(); }
                return G_SOURCE_REMOVE;
            }, this);
        }
    }

    void retire_advertisement() {
        if (!registered_advertisement || stopping) { return; }
        registered_advertisement = false;
        call(bluez_owner.c_str(), adapter.c_str(), "org.bluez.LEAdvertisingManager1", "UnregisterAdvertisement",
            g_variant_new("(o)", kAdvertisement), G_VARIANT_TYPE_UNIT,
            [](Impl &, GVariant *response, GError *error) {
                if (response == nullptr) {
                    std::fprintf(stderr, "Cannot retire Bluetooth advertisement: %s\n",
                        error != nullptr ? error->message : "No reply");
                }
            });
    }

    void fail(const std::string &message) {
        if (!stopping && failure.empty()) { failure = message; }
        stop();
    }

    void cleanup_reply() {
        if (cleanup_pending > 0) { --cleanup_pending; }
        if (cleanup_pending == 0) {
            remove_source(cleanup_timeout);
            g_main_loop_quit(loop);
        }
    }

    void stop() {
        if (stopping) { return; }
        stopping = true;
        g_cancellable_cancel(startup);
        remove_source(startup_timeout);
        remove_source(poll_source);
        remove_source(send_source);
        remove_source(receive_source);
        remove_source(notify_timeout);
        remove_source(release_timeout);
        incoming.clear();
        outgoing.clear();
        if (session != nullptr) { session->close(); }
        if (bus != nullptr && !g_dbus_connection_is_closed(bus) && !bluez_owner.empty()) {
            const auto cleanup = [](Impl &owner, GVariant *, GError *) { owner.cleanup_reply(); };
            if (registered_advertisement) {
                registered_advertisement = false;
                ++cleanup_pending;
                call(bluez_owner.c_str(), adapter.c_str(), "org.bluez.LEAdvertisingManager1", "UnregisterAdvertisement",
                    g_variant_new("(o)", kAdvertisement), G_VARIANT_TYPE_UNIT, cleanup, true);
            }
            if (registered_gatt) {
                registered_gatt = false;
                ++cleanup_pending;
                call(bluez_owner.c_str(), adapter.c_str(), "org.bluez.GattManager1", "UnregisterApplication",
                    g_variant_new("(o)", kRoot), G_VARIANT_TYPE_UNIT, cleanup, true);
            }
        }
        if (cleanup_pending == 0) {
            g_main_loop_quit(loop);
        } else {
            cleanup_timeout = g_timeout_add(3000, [](gpointer data) -> gboolean {
                auto &owner = *static_cast<Impl *>(data);
                owner.cleanup_timeout = 0;
                g_main_loop_quit(owner.loop);
                return G_SOURCE_REMOVE;
            }, this);
        }
    }
};

BluezAgent::BluezAgent(Storage &storage)
    : impl_(std::make_shared<Impl>(storage)) {}

BluezAgent::~BluezAgent() = default;

void BluezAgent::run() {
    impl_->initialize();
    if (!impl_->stopping) { g_main_loop_run(impl_->loop); }
    if (!impl_->failure.empty()) { throw std::runtime_error(impl_->failure); }
}

void BluezAgent::stop() { impl_->stop(); }

}
