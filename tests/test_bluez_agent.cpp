#include "ble_framing.h"

#include <gio/gio.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

constexpr const char *kRoot = "/cc/lilygo/ui/Launcher/share";
constexpr const char *kService = "/cc/lilygo/ui/Launcher/share/service";
constexpr const char *kReceive = "/cc/lilygo/ui/Launcher/share/service/rx";
constexpr const char *kTransmit = "/cc/lilygo/ui/Launcher/share/service/tx";
constexpr const char *kAdvertisement = "/cc/lilygo/ui/Launcher/share_advertisement";
constexpr const char *kAdapter = "/org/bluez/hci0";
constexpr const char *kDevice = "/org/bluez/hci0/dev_01_02_03_04_05_06";
constexpr const char *kOtherDevice = "/org/bluez/hci0/dev_11_12_13_14_15_16";
constexpr const char *kProperties = "org.freedesktop.DBus.Properties";
constexpr const char *kCharacteristic = "org.bluez.GattCharacteristic1";
constexpr const char *kObjectManager = "org.freedesktop.DBus.ObjectManager";
constexpr const char *kMockXml = R"xml(
<node>
  <interface name="org.freedesktop.DBus.ObjectManager">
    <method name="GetManagedObjects"><arg type="a{oa{sa{sv}}}" direction="out"/></method>
  </interface>
  <interface name="org.bluez.Adapter1">
    <property name="Powered" type="b" access="readwrite"/>
    <property name="Alias" type="s" access="readwrite"/>
  </interface>
  <interface name="org.bluez.GattManager1">
    <method name="RegisterApplication"><arg type="o" direction="in"/><arg type="a{sv}" direction="in"/></method>
    <method name="UnregisterApplication"><arg type="o" direction="in"/></method>
  </interface>
  <interface name="org.bluez.LEAdvertisingManager1">
    <method name="RegisterAdvertisement"><arg type="o" direction="in"/><arg type="a{sv}" direction="in"/></method>
    <method name="UnregisterAdvertisement"><arg type="o" direction="in"/></method>
  </interface>
</node>)xml";

struct VariantDeleter {
    void operator()(GVariant *value) const { g_variant_unref(value); }
};
using Variant = std::unique_ptr<GVariant, VariantDeleter>;
using Json = nlohmann::json;

void require(bool condition, const std::string &message)
{
    if (!condition) throw std::runtime_error(message);
}

std::string error_message(GError *error)
{
    const std::string result = error ? error->message : "No error details";
    g_clear_error(&error);
    return result;
}

Variant call(GDBusConnection *connection, const char *destination, const char *path,
             const char *interface, const char *method, GVariant *parameters = nullptr,
             const GVariantType *reply_type = G_VARIANT_TYPE_UNIT, const char *expected_error = nullptr)
{
    GError *error = nullptr;
    Variant result(g_dbus_connection_call_sync(connection, destination, path, interface, method, parameters,
                   reply_type, G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, &error));
    if (expected_error) {
        gchar *remote = error ? g_dbus_error_get_remote_error(error) : nullptr;
        const bool matches = remote && std::strcmp(remote, expected_error) == 0;
        g_free(remote);
        const std::string details = error ? error_message(error) : "Method succeeded";
        require(!result && matches, std::string(method) + " expected " + expected_error + ": " + details);
    } else {
        const std::string details = error ? error_message(error) : std::string();
        require(result != nullptr, std::string(method) + " failed: " + details);
    }
    return result;
}

Variant property(GVariant *dictionary, const char *name, const GVariantType *type)
{
    Variant value(g_variant_lookup_value(dictionary, name, type));
    require(value != nullptr, std::string("Missing property or wrong type: ") + name);
    return value;
}

void string_property(GVariant *dictionary, const char *name, const char *expected,
                     const GVariantType *type = G_VARIANT_TYPE_STRING)
{
    const auto value = property(dictionary, name, type);
    require(std::strcmp(g_variant_get_string(value.get(), nullptr), expected) == 0,
            std::string("Unexpected property: ") + name);
}

GVariant *empty_dictionary()
{
    return g_variant_new_array(G_VARIANT_TYPE("{sv}"), nullptr, 0);
}

std::string encode_length(std::uint32_t size)
{
    std::string result(4, '\0');
    for (unsigned index = 0; index < 4; ++index)
        result[index] = static_cast<char>(size >> (24 - 8 * index));
    return result;
}

class Fixture {
public:
    Fixture(std::string address, std::string executable, bool reject_advertisement = false)
        : address_(std::move(address)), executable_(std::move(executable)),
          reject_advertisement_(reject_advertisement) {}

    ~Fixture()
    {
        if (child_ != 0 && !exited_) {
            kill(child_, SIGKILL);
            while (waitpid(child_, nullptr, 0) < 0 && errno == EINTR) {}
        }
        if (child_ != 0) g_spawn_close_pid(child_);
        if (ipc_ >= 0) close(ipc_);
        if (bluez_) {
            if (notification_subscription_)
                g_dbus_connection_signal_unsubscribe(bluez_, notification_subscription_);
            for (guint registration : registrations_)
                g_dbus_connection_unregister_object(bluez_, registration);
        }
        for (GDBusConnection *connection : {attacker_, bluez_}) {
            if (!connection) continue;
            g_dbus_connection_close_sync(connection, nullptr, nullptr);
            g_object_unref(connection);
        }
        if (introspection_) g_dbus_node_info_unref(introspection_);
        while (g_main_context_iteration(nullptr, false)) {}
    }

    void start()
    {
        GError *error = nullptr;
        introspection_ = g_dbus_node_info_new_for_xml(kMockXml, &error);
        require(introspection_ != nullptr, error ? error_message(error) : "Invalid mock introspection");
        bluez_ = connect_bus();
        attacker_ = connect_bus();
        const auto ownership = call(bluez_, "org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus", "RequestName", g_variant_new("(su)", "org.bluez", 4U), G_VARIANT_TYPE("(u)"));
        guint32 ownership_result = 0;
        g_variant_get(ownership.get(), "(u)", &ownership_result);
        require(ownership_result == 1, "Cannot own the fake org.bluez service");
        register_interface("/", kObjectManager);
        for (const char *interface : {"org.bluez.Adapter1", "org.bluez.GattManager1", "org.bluez.LEAdvertisingManager1"})
            register_interface(kAdapter, interface);
        notification_subscription_ = g_dbus_connection_signal_subscribe(bluez_, nullptr, kProperties,
            "PropertiesChanged", kTransmit, kCharacteristic, G_DBUS_SIGNAL_FLAGS_NONE,
            [](GDBusConnection *, const gchar *sender, const gchar *, const gchar *, const gchar *,
               GVariant *parameters, gpointer data) {
                auto &fixture = *static_cast<Fixture *>(data);
                if (fixture.helper_ != sender) return;
                try {
                    Variant changed(g_variant_get_child_value(parameters, 1));
                    Variant bytes(g_variant_lookup_value(changed.get(), "Value", G_VARIANT_TYPE("ay")));
                    if (!bytes) return;
                    gsize size = 0;
                    const auto *value = static_cast<const std::uint8_t *>(g_variant_get_fixed_array(bytes.get(), &size, 1));
                    require(size >= 5 && size <= 20, "Notifications must fit the minimum ATT MTU");
                    const auto envelope = fixture.decoder_.feed({value, value + size});
                    if (envelope) fixture.notifications_.push_back(*envelope);
                } catch (const std::exception &exception) {
                    fixture.failure_ = exception.what();
                }
            }, this, nullptr);
        int sockets[2];
        require(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "Cannot create the helper IPC fixture");
        ipc_ = sockets[0];
        require(fcntl(ipc_, F_SETFL, O_NONBLOCK) == 0, "Cannot make test IPC nonblocking");
        const gchar *arguments[] = {executable_.c_str(), "--ipc-fd", "3", nullptr};
        gchar **environment = g_get_environ();
        environment = g_environ_setenv(environment, "DBUS_SYSTEM_BUS_ADDRESS", address_.c_str(), true);
        const gint sources[] = {sockets[1]};
        const gint destinations[] = {3};
        const bool spawned = g_spawn_async_with_pipes_and_fds(nullptr, arguments, environment,
            G_SPAWN_DO_NOT_REAP_CHILD, nullptr, nullptr, -1, -1, -1, sources, destinations, 1,
            &child_, nullptr, nullptr, nullptr, &error);
        g_strfreev(environment);
        close(sockets[1]);
        require(spawned, error ? error_message(error) : "Cannot spawn the native helper");
    }

    void wait_ready()
    {
        wait_for([this] { return registered_application_ && registered_advertisement_ &&
            std::find(statuses_.begin(), statuses_.end(), "Connect to lilygo_eeff from the browser on the same network.") != statuses_.end(); },
            "GATT and advertisement registration");
        require(powered_ && checked_schema_ && checked_advertisement_, "BlueZ registration schema was not verified");
    }

    void invoke(const char *path, const char *interface, const char *method,
                bool attacker = false, const char *expected_error = nullptr)
    {
        call(attacker ? attacker_ : bluez_, helper_.c_str(), path, interface, method,
             nullptr, G_VARIANT_TYPE_UNIT, expected_error);
    }

    void write_frame(const std::vector<std::uint8_t> &frame, const char *device = kDevice,
                     guint16 offset = 0, bool attacker = false, const char *expected_error = nullptr)
    {
        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
        if (device) g_variant_builder_add(&options, "{sv}", "device", g_variant_new_object_path(device));
        g_variant_builder_add(&options, "{sv}", "offset", g_variant_new_uint16(offset));
        call(attacker ? attacker_ : bluez_, helper_.c_str(), kReceive, kCharacteristic, "WriteValue",
             g_variant_new("(@ay@a{sv})", g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, frame.data(), frame.size(), 1),
                           g_variant_builder_end(&options)), G_VARIANT_TYPE_UNIT, expected_error);
    }

    std::vector<std::uint8_t> read_value(guint16 offset = 0, bool attacker = false,
                                       const char *expected_error = nullptr)
    {
        GVariantBuilder options;
        g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&options, "{sv}", "offset", g_variant_new_uint16(offset));
        const auto response = call(attacker ? attacker_ : bluez_, helper_.c_str(), kTransmit,
            kCharacteristic, "ReadValue", g_variant_new("(@a{sv})", g_variant_builder_end(&options)),
            G_VARIANT_TYPE("(ay)"), expected_error);
        if (!response) return {};
        Variant value(g_variant_get_child_value(response.get(), 0));
        gsize size = 0;
        const auto *bytes = static_cast<const std::uint8_t *>(g_variant_get_fixed_array(value.get(), &size, 1));
        return size == 0 ? std::vector<std::uint8_t>() : std::vector<std::uint8_t>(bytes, bytes + size);
    }

    void send_signal(const std::string &type, const Json &payload = Json::object())
    {
        for (const auto &frame : launcher::transport::encode_message(envelope(type, payload)))
            write_frame(frame);
    }

    Json wait_signal(const std::string &type)
    {
        wait_for([this, &type] {
            return std::any_of(notifications_.begin(), notifications_.end(), [&type](const Json &message) {
                return message.value("type", std::string()) == type;
            });
        }, "Bluetooth notification " + type);
        const auto found = std::find_if(notifications_.begin(), notifications_.end(), [&type](const Json &message) {
            return message.value("type", std::string()) == type;
        });
        const auto result = *found;
        notifications_.erase(found);
        require(result.at("sessionId") == "mock-session" && result.at("protocolVersion") == 1,
                "Notifications must preserve the session identity and protocol version");
        return result;
    }

    bool notifying()
    {
        const auto response = call(bluez_, helper_.c_str(), kTransmit, kProperties, "Get",
            g_variant_new("(ss)", kCharacteristic, "Notifying"), G_VARIANT_TYPE("(v)"));
        Variant wrapped(g_variant_get_child_value(response.get(), 0));
        Variant value(g_variant_get_variant(wrapped.get()));
        return g_variant_get_boolean(value.get());
    }

    void release_bluez_name()
    {
        call(bluez_, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
             "ReleaseName", g_variant_new("(s)", "org.bluez"), G_VARIANT_TYPE("(u)"));
    }

    void wait_exit(int expected_code)
    {
        wait_for([this] { return exited_; }, "helper shutdown");
        require(WIFEXITED(exit_status_) && WEXITSTATUS(exit_status_) == expected_code,
                "The native helper returned an unexpected exit status");
        require(cancels_ > 0, "The native helper did not cancel storage during cleanup");
    }

    unsigned application_unregistrations() const { return application_unregistrations_; }
    unsigned advertisement_unregistrations() const { return advertisement_unregistrations_; }

    bool has_status(const std::string &needle) const
    {
        return std::any_of(statuses_.begin(), statuses_.end(), [&needle](const std::string &status) {
            return status.find(needle) != std::string::npos;
        });
    }

    static Json envelope(const std::string &type, const Json &payload = Json::object())
    {
        return {{"type", type}, {"sessionId", "mock-session"}, {"protocolVersion", 1}, {"payload", payload}};
    }

private:
    GDBusConnection *connect_bus()
    {
        GError *error = nullptr;
        auto *connection = g_dbus_connection_new_for_address_sync(address_.c_str(),
            static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                              G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
            nullptr, nullptr, &error);
        require(connection != nullptr, error ? error_message(error) : "Cannot connect to the private D-Bus");
        g_dbus_connection_set_exit_on_close(connection, false);
        return connection;
    }

    void register_interface(const char *path, const char *interface)
    {
        static const GDBusInterfaceVTable vtable{
            [](GDBusConnection *, const gchar *sender, const gchar *, const gchar *, const gchar *method,
               GVariant *parameters, GDBusMethodInvocation *invocation, gpointer data) {
                auto &fixture = *static_cast<Fixture *>(data);
                try {
                    fixture.method(sender, method, parameters, invocation);
                } catch (const std::exception &exception) {
                    fixture.failure_ = exception.what();
                    g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Failed", exception.what());
                }
            },
            [](GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *name,
               GError **, gpointer data) -> GVariant * {
                auto &fixture = *static_cast<Fixture *>(data);
                if (std::strcmp(name, "Powered") == 0)
                    return g_variant_new_boolean(fixture.powered_);
                if (std::strcmp(name, "Alias") == 0)
                    return g_variant_new_string(fixture.alias_.c_str());
                return nullptr;
            },
            [](GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *name,
               GVariant *value, GError **, gpointer data) -> gboolean {
                auto &fixture = *static_cast<Fixture *>(data);
                if (std::strcmp(name, "Powered") == 0 && g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN)) {
                    fixture.powered_ = g_variant_get_boolean(value);
                    return true;
                }
                if (std::strcmp(name, "Alias") == 0 && g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)) {
                    fixture.alias_ = g_variant_get_string(value, nullptr);
                    return true;
                }
                return false;
            }, {}
        };
        GError *error = nullptr;
        const guint registration = g_dbus_connection_register_object(bluez_, path,
            g_dbus_node_info_lookup_interface(introspection_, interface), &vtable, this, nullptr, &error);
        require(registration != 0, error ? error_message(error) : "Cannot export fake BlueZ");
        registrations_.push_back(registration);
    }

    void method(const char *sender, const char *method, GVariant *parameters, GDBusMethodInvocation *invocation)
    {
        const std::string name(method);
        if (name == "GetManagedObjects") {
            GVariantBuilder interfaces;
            g_variant_builder_init(&interfaces, G_VARIANT_TYPE("a{sa{sv}}"));
            GVariantBuilder adapter_properties;
            g_variant_builder_init(&adapter_properties, G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(&adapter_properties, "{sv}", "Address",
                                  g_variant_new_string("AA:BB:CC:DD:EE:FF"));
            g_variant_builder_add(&adapter_properties, "{sv}", "Alias",
                                  g_variant_new_string("raspberrypi"));
            g_variant_builder_add(&interfaces, "{s@a{sv}}", "org.bluez.Adapter1",
                                  g_variant_builder_end(&adapter_properties));
            for (const char *interface : {"org.bluez.GattManager1", "org.bluez.LEAdvertisingManager1"})
                g_variant_builder_add(&interfaces, "{s@a{sv}}", interface, empty_dictionary());
            GVariantBuilder objects;
            g_variant_builder_init(&objects, G_VARIANT_TYPE("a{oa{sa{sv}}}"));
            g_variant_builder_add(&objects, "{o@a{sa{sv}}}", kAdapter, g_variant_builder_end(&interfaces));
            g_dbus_method_invocation_return_value(invocation,
                g_variant_new("(@a{oa{sa{sv}}})", g_variant_builder_end(&objects)));
            return;
        }
        Variant path_value(g_variant_get_child_value(parameters, 0));
        const std::string path = g_variant_get_string(path_value.get(), nullptr);
        if (name == "RegisterApplication") {
            require(path == kRoot && powered_, "Registration must export the expected root and power the adapter");
            helper_ = sender;
            check_schema();
            registered_application_ = true;
        } else if (name == "RegisterAdvertisement") {
            require(path == kAdvertisement && registered_application_, "Advertisement registration order or path is wrong");
            check_advertisement();
            if (reject_advertisement_) {
                g_dbus_method_invocation_return_dbus_error(invocation, "org.bluez.Error.Failed", "Mock advertising failure");
                return;
            }
            registered_advertisement_ = true;
        } else if (name == "UnregisterApplication") {
            require(path == kRoot && registered_application_, "Unexpected GATT cleanup");
            registered_application_ = false;
            ++application_unregistrations_;
        } else if (name == "UnregisterAdvertisement") {
            require(path == kAdvertisement && registered_advertisement_, "Unexpected advertisement cleanup");
            registered_advertisement_ = false;
            ++advertisement_unregistrations_;
        } else {
            throw std::runtime_error("Unexpected fake BlueZ method: " + name);
        }
        g_dbus_method_invocation_return_value(invocation, nullptr);
    }

    void check_introspection(const char *path, bool receive)
    {
        const auto response = call(bluez_, helper_.c_str(), path, "org.freedesktop.DBus.Introspectable",
            "Introspect", nullptr, G_VARIANT_TYPE("(s)"));
        const gchar *xml = nullptr;
        g_variant_get(response.get(), "(&s)", &xml);
        GError *error = nullptr;
        GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(xml, &error);
        require(node != nullptr, error ? error_message(error) : "Helper introspection is invalid");
        std::unique_ptr<GDBusNodeInfo, decltype(&g_dbus_node_info_unref)> owner(node, g_dbus_node_info_unref);
        GDBusInterfaceInfo *interface = g_dbus_node_info_lookup_interface(node, kCharacteristic);
        require(interface != nullptr, "GATT characteristic interface is missing");
        GDBusMethodInfo *write = g_dbus_interface_info_lookup_method(interface, "WriteValue");
        GDBusMethodInfo *read = g_dbus_interface_info_lookup_method(interface, "ReadValue");
        GDBusMethodInfo *start = g_dbus_interface_info_lookup_method(interface, "StartNotify");
        GDBusMethodInfo *stop = g_dbus_interface_info_lookup_method(interface, "StopNotify");
        if (receive) {
            require(write && !read && !start && !stop && write->in_args && write->in_args[0] && write->in_args[1] && !write->in_args[2],
                    "RX must expose exactly the two WriteValue inputs");
            require(std::strcmp(write->in_args[0]->signature, "ay") == 0 &&
                    std::strcmp(write->in_args[1]->signature, "a{sv}") == 0, "Wrong WriteValue D-Bus signature");
        } else {
            require(!write && read && start && stop && read->in_args && read->in_args[0] && read->out_args && read->out_args[0] &&
                    std::strcmp(read->in_args[0]->signature, "a{sv}") == 0 &&
                    std::strcmp(read->out_args[0]->signature, "ay") == 0 &&
                    (!start->in_args || !start->in_args[0]) &&
                    (!stop->in_args || !stop->in_args[0]), "TX must expose no-argument notification methods");
        }
    }

    void check_schema()
    {
        const auto response = call(bluez_, helper_.c_str(), kRoot, kObjectManager, "GetManagedObjects",
            nullptr, G_VARIANT_TYPE("(a{oa{sa{sv}}})"));
        Variant objects(g_variant_get_child_value(response.get(), 0));
        require(g_variant_n_children(objects.get()) == 3, "The application must export one service and two characteristics");
        for (const char *path : {kService, kReceive, kTransmit}) {
            const auto interfaces = property(objects.get(), path, G_VARIANT_TYPE("a{sa{sv}}"));
            const bool service = std::strcmp(path, kService) == 0;
            const bool receive = std::strcmp(path, kReceive) == 0;
            const auto properties = property(interfaces.get(), service ? "org.bluez.GattService1" : kCharacteristic,
                G_VARIANT_TYPE("a{sv}"));
            string_property(properties.get(), "UUID", service ? launcher::transport::kServiceUuid :
                receive ? launcher::transport::kRxUuid : launcher::transport::kTxUuid);
            if (service) {
                const auto primary = property(properties.get(), "Primary", G_VARIANT_TYPE_BOOLEAN);
                require(g_variant_get_boolean(primary.get()), "The sharing GATT service must be primary");
                const auto includes = property(properties.get(), "Includes", G_VARIANT_TYPE("ao"));
                require(g_variant_n_children(includes.get()) == 0, "Unexpected included GATT services");
            } else {
                string_property(properties.get(), "Service", kService, G_VARIANT_TYPE_OBJECT_PATH);
                const auto flags = property(properties.get(), "Flags", G_VARIANT_TYPE("as"));
                require(g_variant_n_children(flags.get()) == 2, "Unexpected GATT characteristic flags");
                const char *expected_flags[] = {receive ? "write" : "read", receive ? "write-without-response" : "notify"};
                for (unsigned index = 0; index < 2; ++index) {
                    Variant flag(g_variant_get_child_value(flags.get(), index));
                    require(std::strcmp(g_variant_get_string(flag.get(), nullptr), expected_flags[index]) == 0,
                            "Wrong GATT characteristic capability");
                }
                if (!receive) {
                    const auto active = property(properties.get(), "Notifying", G_VARIANT_TYPE_BOOLEAN);
                    require(!g_variant_get_boolean(active.get()), "TX must start unsubscribed");
                    const auto value = property(properties.get(), "Value", G_VARIANT_TYPE("ay"));
                    require(g_variant_n_children(value.get()) == 0, "TX must start with an empty value");
                }
            }
        }
        check_introspection(kReceive, true);
        check_introspection(kTransmit, false);
        checked_schema_ = true;
    }

    void check_advertisement()
    {
        const auto response = call(bluez_, helper_.c_str(), kAdvertisement, kProperties, "GetAll",
            g_variant_new("(s)", "org.bluez.LEAdvertisement1"), G_VARIANT_TYPE("(a{sv})"));
        Variant properties(g_variant_get_child_value(response.get(), 0));
        string_property(properties.get(), "Type", "peripheral");
        string_property(properties.get(), "LocalName", "lilygo_eeff");
        const auto uuids = property(properties.get(), "ServiceUUIDs", G_VARIANT_TYPE("as"));
        require(g_variant_n_children(uuids.get()) == 1, "Advertisement must carry one service UUID");
        Variant uuid(g_variant_get_child_value(uuids.get(), 0));
        require(std::strcmp(g_variant_get_string(uuid.get(), nullptr), launcher::transport::kServiceUuid) == 0,
                "Advertisement service UUID does not match GATT");
        const auto discoverable = property(properties.get(), "Discoverable", G_VARIANT_TYPE_BOOLEAN);
        require(g_variant_get_boolean(discoverable.get()), "Advertisement must be discoverable");
        checked_advertisement_ = true;
    }

    void pump_ipc()
    {
        char bytes[4096];
        while (true) {
            const ssize_t count = recv(ipc_, bytes, sizeof(bytes), 0);
            if (count > 0) {
                ipc_bytes_.append(bytes, static_cast<std::size_t>(count));
            } else if (count < 0 && errno == EINTR) {
                continue;
            } else {
                require(count == 0 || errno == EAGAIN || errno == EWOULDBLOCK, "IPC fixture receive failed");
                break;
            }
        }
        while (ipc_bytes_.size() >= 4) {
            std::uint32_t size = 0;
            for (unsigned index = 0; index < 4; ++index)
                size = (size << 8) | static_cast<unsigned char>(ipc_bytes_[index]);
            require(size > 0 && size <= 1024 * 1024 + 9, "Invalid storage request length");
            if (ipc_bytes_.size() < size + 4) break;
            const std::string frame = ipc_bytes_.substr(4, size);
            ipc_bytes_.erase(0, size + 4);
            if (frame[0] == 'S') statuses_.push_back(frame.substr(1));
            else if (frame[0] == 'C') ++cancels_;
            else throw std::runtime_error("Unexpected file operation before WebRTC negotiation");
            const std::string response = encode_length(2) + std::string("R\1", 2);
            require(send(ipc_, response.data(), response.size(), 0) == static_cast<ssize_t>(response.size()),
                    "Cannot acknowledge storage IPC");
        }
    }

    void wait_for(const std::function<bool()> &predicate, const std::string &operation)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (!predicate()) {
            while (g_main_context_iteration(nullptr, false)) {}
            pump_ipc();
            require(failure_.empty(), failure_);
            if (!exited_) {
                const pid_t result = waitpid(child_, &exit_status_, WNOHANG);
                require(result >= 0, "Cannot reap the native helper");
                exited_ = result == child_;
            }
            require(std::chrono::steady_clock::now() < deadline, "Timed out waiting for " + operation);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    std::string address_;
    std::string executable_;
    bool reject_advertisement_;
    GDBusConnection *bluez_ = nullptr;
    GDBusConnection *attacker_ = nullptr;
    GDBusNodeInfo *introspection_ = nullptr;
    std::vector<guint> registrations_;
    guint notification_subscription_ = 0;
    GPid child_ = 0;
    int ipc_ = -1;
    int exit_status_ = 0;
    bool exited_ = false;
    bool powered_ = false;
    std::string alias_ = "raspberrypi";
    bool registered_application_ = false;
    bool registered_advertisement_ = false;
    bool checked_schema_ = false;
    bool checked_advertisement_ = false;
    unsigned application_unregistrations_ = 0;
    unsigned advertisement_unregistrations_ = 0;
    unsigned cancels_ = 0;
    std::string helper_;
    std::string failure_;
    std::string ipc_bytes_;
    std::vector<std::string> statuses_;
    launcher::transport::FrameDecoder decoder_;
    std::vector<Json> notifications_;
};

void test_connection_and_owner_gating(const std::string &address, const std::string &executable)
{
    Fixture fixture(address, executable);
    fixture.start();
    fixture.wait_ready();
    const auto frames = launcher::transport::encode_message(Fixture::envelope("session.hello"));
    require(fixture.read_value().empty(), "TX reads before the first notification must be empty");
    fixture.read_value(1, false, "org.bluez.Error.InvalidOffset");
    fixture.read_value(0, true, "org.bluez.Error.NotAuthorized");
    fixture.invoke(kTransmit, kCharacteristic, "StartNotify", true, "org.bluez.Error.NotAuthorized");
    fixture.invoke(kAdvertisement, "org.bluez.LEAdvertisement1", "Release", true, "org.bluez.Error.NotAuthorized");
    fixture.write_frame(frames.front(), kDevice, 0, true, "org.bluez.Error.NotAuthorized");
    require(!fixture.notifying(), "An unauthorized caller enabled notifications");
    fixture.write_frame(frames.front(), kDevice, 0, false, "org.bluez.Error.NotPermitted");
    fixture.write_frame(frames.front(), kDevice, 1, false, "org.bluez.Error.InvalidOffset");
    fixture.invoke(kTransmit, kCharacteristic, "StartNotify");
    require(fixture.notifying(), "BlueZ did not enable notifications");
    fixture.invoke(kTransmit, kCharacteristic, "StartNotify");
    require(fixture.notifying(), "Repeated StartNotify must preserve the active subscription");
    fixture.write_frame(frames.front(), nullptr, 0, false, "org.bluez.Error.NotPermitted");
    fixture.write_frame({1, 0, 0, 0}, kDevice, 0, false, "org.bluez.Error.Failed");
    fixture.write_frame(frames.front());
    fixture.write_frame(frames.front(), kOtherDevice, 0, false, "org.bluez.Error.NotAuthorized");
    for (std::size_t index = 1; index < frames.size(); ++index) fixture.write_frame(frames[index]);
    const auto accepted = fixture.wait_signal("session.accepted");
    require(accepted.at("payload").at("pairingRequired") == false &&
            accepted.at("payload").at("transport") == "ble-webrtc", "Hello must connect immediately");
    const auto last_frame = launcher::transport::encode_message(accepted).back();
    require(fixture.read_value() == last_frame, "ReadValue must return the current notification frame");
    require(fixture.read_value(4) == std::vector<std::uint8_t>(last_frame.begin() + 4, last_frame.end()),
            "ReadValue must honor an offset within the current frame");
    require(fixture.read_value(static_cast<guint16>(last_frame.size())).empty(),
            "Reading at the end of the frame must return an empty value");
    fixture.read_value(static_cast<guint16>(last_frame.size() + 1), false, "org.bluez.Error.InvalidOffset");
    fixture.send_signal("session.close");
    fixture.wait_exit(0);
    require(fixture.application_unregistrations() == 1 && fixture.advertisement_unregistrations() == 1,
            "Closing a session must unregister both BlueZ objects");
}

void test_registration_failure(const std::string &address, const std::string &executable)
{
    Fixture fixture(address, executable, true);
    fixture.start();
    fixture.wait_exit(1);
    require(fixture.application_unregistrations() == 1 && fixture.advertisement_unregistrations() == 0,
            "Failed advertisement registration must unwind the registered application");
    require(fixture.has_status("Mock advertising failure"), "Registration failure did not reach launcher status");
}

void test_bluez_owner_loss(const std::string &address, const std::string &executable)
{
    Fixture fixture(address, executable);
    fixture.start();
    fixture.wait_ready();
    fixture.release_bluez_name();
    fixture.wait_exit(0);
    require(fixture.application_unregistrations() == 0 && fixture.advertisement_unregistrations() == 0,
            "The helper must retire registrations when the BlueZ owner disappears");
}

void test_advertisement_release(const std::string &address, const std::string &executable)
{
    Fixture fixture(address, executable);
    fixture.start();
    fixture.wait_ready();
    fixture.invoke(kAdvertisement, "org.bluez.LEAdvertisement1", "Release");
    fixture.wait_exit(0);
    require(fixture.application_unregistrations() == 1 && fixture.advertisement_unregistrations() == 0,
            "Released advertisement must still unwind the GATT application");
}

void test_unwritten_notification_cleanup(const std::string &address, const std::string &executable)
{
    Fixture fixture(address, executable);
    fixture.start();
    fixture.wait_ready();
    fixture.invoke(kTransmit, kCharacteristic, "StartNotify");
    fixture.invoke(kTransmit, kCharacteristic, "StopNotify");
    fixture.wait_exit(0);
    require(fixture.application_unregistrations() == 1 && fixture.advertisement_unregistrations() == 1,
            "Stopping notifications before the first write must clean up both BlueZ registrations");
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "Usage: %s /absolute/lilygo-ui-file-share-agent\n", argv[0]);
        return 2;
    }
    const char *configured_daemon = g_getenv("G_TEST_DBUS_DAEMON");
    gchar *daemon = configured_daemon ? g_strdup(configured_daemon) : g_find_program_in_path("dbus-daemon");
    if (!daemon || access(daemon, X_OK) != 0) {
        g_free(daemon);
        std::fprintf(stderr, "D-Bus integration test skipped: dbus-daemon is unavailable\n");
        return 77;
    }
    g_free(daemon);
    signal(SIGPIPE, SIG_IGN);
    GTestDBus *bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(bus);
    int result = 0;
    try {
        const std::string address = g_test_dbus_get_bus_address(bus);
        test_connection_and_owner_gating(address, argv[1]);
        test_registration_failure(address, argv[1]);
        test_bluez_owner_loss(address, argv[1]);
        test_advertisement_release(address, argv[1]);
        test_unwritten_notification_cleanup(address, argv[1]);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Native BlueZ integration test failed: %s\n", error.what());
        result = 1;
    }
    g_test_dbus_down(bus);
    g_object_unref(bus);
    return result;
}
