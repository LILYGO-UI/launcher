# Browser LAN file sharing

Launcher receives browser files over a local WebRTC DataChannel. Bluetooth
discovers the device and carries fast connection negotiation. File bytes
travel over the LAN connection; no WSS signaling server, STUN server, TURN relay,
or public upload service is required.

The browser client belongs to the independent `lilygo-ui.github.io` repository.
Its pages are `/file-share.html` and `/zh/file-share.html`. Launcher owns the
device transport, authenticated account boundary, and Downloads storage.

## Connect and send

1. Connect the browser's computer and the LILYGO device to the same LAN. The
   devices must be able to reach each other; access-point client isolation can
   prevent the direct connection.
2. Sign in on the device and open **Quick controls → File sharing**. Keep the
   panel open. It displays the destination directory and waits for a browser.
3. Open the sharing page over HTTPS or localhost in a browser with Web Bluetooth,
   such as a supported desktop Chrome or Edge installation. Click **连接附近设备**
   and select the device named `lilygo_xxxx` (the last two Bluetooth MAC
   bytes). The browser automatically discovers the
   service, subscribes to notifications, and negotiates a direct local-network
   connection. Once the DataChannel opens, Bluetooth disconnects automatically.
   No code entry or signaling-server address is required.
4. Choose **选择文件发送**. Before sending any file bytes or creating a
   destination file, the device displays that file's name and size. Touch
   **Receive file** on the device to allow this transfer, or **Decline** to
   reject it. With a hardware keyboard, use the direction keys to select an
   action and Enter to confirm; each new request starts with **Decline** selected.
   The browser waits for this decision before sending file contents.
   Each subsequent file requires a new device confirmation. Wait for the
   browser's completed receipt before treating a file as saved; **取消传输**
   removes the incomplete file, and **断开连接** ends the connection.

The device's explicit per-file confirmation is the authorization boundary:
connecting or negotiating a session alone never permits file data to be stored.
Closing the device panel, locking, signing out, or exiting Launcher stops the
session and cancels incomplete storage.

Sharing is enabled only while the signed-in user's panel is open. Closing the
panel with **Close** or Esc, locking, signing out, launching another application,
or exiting Launcher stops the transport and cancels incomplete storage. The
Bluetooth service is not advertised on the sign-in screen. After the
DataChannel opens, the device also stops advertising for new browsers while
continuing the existing transfer session. The simulator shows the panel but
does not start the device transport by default.

## Device architecture and installed files

`src/file_share_receiver.cpp` supervises a private C++ transport process:

```text
/usr/lib/lilygo-ui-launcher/file-share/lilygo-ui-file-share-agent \
  --ipc-fd 3
```

The helper implements BlueZ GATT/advertising through GDBus and the local
WebRTC peer through libdatachannel. BlueZ runs on the system D-Bus; the helper
exports its GATT and advertisement objects and registers them with BlueZ. The
helper inherits its socket, standard output/error for journald, and an empty
standard input. It does not inherit Launcher device descriptors. A Linux
parent-death signal terminates it if Launcher crashes.

The C++ receiver snapshots the authenticated `launcher_user_t`. A dedicated
worker processes one IPC request at a time and performs all file writes, hashing,
and final synchronization outside the LVGL thread. Main-thread status snapshots
update the existing sharing panel. Shutdown closes the socket, joins the storage
worker, terminates/reaps the helper, and removes partial files.

The transport runtime depends on `bluez`, `libglib2.0-0t64`, and `libssl3t64`.
It does not require Python. CMake builds libdatachannel 0.23.2 from the pinned
commit `9e6a13abbb6846c003d817d0387b6706466e2b03`, including its pinned
libjuice, usrsctp, and plog submodules; nlohmann JSON comes from the same
checkout. These dependencies are linked statically with media, WebSocket,
upstream examples, and upstream tests disabled. Their license files are
installed under `/usr/share/doc/lilygo-ui-launcher/transport`.

Native build hosts need the GLib/GIO and OpenSSL development files
(`libglib2.0-dev`, `libssl-dev`, and `pkg-config` on Debian). For the default
CM0 BSP 0.1.0, CMake downloads SHA-256-pinned ARM64 development packages,
including the sysprof and zlib metadata required by GLib, into
`.cache/cm0-bsp/transport/trixie-arm64-v1`. This separate cache links to the
BSP's runtime libraries without modifying the original sysroot. Custom BSPs
must supply their own matching development files. Once populated, the cache
is reused without downloading the packages again. An offline build can also
set `FETCHCONTENT_SOURCE_DIR_LILYGO_DATACHANNEL` to an existing
checkout of that exact commit with its required submodules initialized.

The native transport can also be built and tested independently of the UI:

```sh
cmake -S transport -B build/transport -DBUILD_TESTING=ON
cmake --build build/transport --parallel
ctest --test-dir build/transport --output-on-failure
```

## Bluetooth signaling

The advertisement name is `lilygo_xxxx`, where `xxxx` is the last two Bluetooth
MAC bytes in lowercase. The browser discovers the fixed service UUID first,
then verifies the dynamic name before subscribing to TX and writing RX:

| Role | UUID | GATT operation |
| --- | --- | --- |
| Service | `fde6eb6b-0d5f-49c1-b7ae-296288a7ab0b` | Primary service |
| RX, browser → device | `fde6eb6c-0d5f-49c1-b7ae-296288a7ab0b` | Write or write without response |
| TX, device → browser | `fde6eb6d-0d5f-49c1-b7ae-296288a7ab0b` | Read, notify |

Each characteristic value is at most 20 bytes, including this four-byte header:

```text
uint8 flags       bit 0: start, bit 1: end
uint8 reserved    must be zero
uint16 sequence   big endian, starts at zero for each message
byte[] payload    1–16 bytes of a UTF-8 JSON envelope
```

A message is limited to 65,536 bytes. Frames must arrive in order; an
incomplete message expires after 15 seconds without a frame. Invalid headers,
UTF-8, JSON, ordering, interleaved starts, and oversized messages are rejected.
The helper binds even partial messages to one BlueZ device path.

Envelopes use `protocolVersion: 1`, a `sessionId`, a `type`, and an object
`payload`. The exchange is:

```text
browser                         device
session.hello                →  session.accepted
sdp.offer { description }    →  sdp.answer { description }
session.close                →  transport cleanup (while BLE is connected)
```

Both peers use an empty ICE-server list and gather their local candidates before
sending SDP. The device accepts only a single application/DataChannel media
section with host candidates. There is no trickle ICE or relay fallback. The
browser may use local mDNS host candidates. Bluetooth only establishes this
connection: after receiving the complete answer and opening the DataChannel,
the browser disconnects GATT and the device withdraws its advertisement. The
authenticated WebRTC peer continues independently over the LAN. Bluetooth link
loss before the DataChannel is established aborts negotiation; releasing
Bluetooth after establishment does not cancel a file.

The browser's disconnect action closes WebRTC. A lost or closed DataChannel
ends the session and removes incomplete storage, as do device-panel closure,
locking, logout, application launch, or Launcher exit.

## File and storage protocol

The single DataChannel is named `file-share`, ordered, and reliable. Control
messages are JSON. `file-start` carries `metadata` with `sessionId`, `fileId`,
`fileName`, `fileSize`, `chunkSize`, and lowercase hexadecimal `sha256`.
The device validates the metadata, displays the confirmation in its UI, and
sends `file-pending` with the file ID, name, and size to the browser.
Only after the user accepts does the receiver open storage and respond
`file-accepted`; the browser sends no binary data before that response. A
decline or confirmation timeout returns `file-error` and creates no file.
Device confirmation expires after 45 seconds; the browser allows 50 seconds
for the decision and its response. Cancellation and disconnect remain
available while confirmation is pending.

Binary chunks contain a four-byte big-endian sequence number followed by bytes.
The browser uses 16,380 payload bytes per chunk; the receiver permits up to
16,384 payload bytes. The browser allows at most 32 unacknowledged chunks.
`file-progress` returns the stored chunk count and byte count every 32 chunks
and after the final chunk. The receiver serializes storage acknowledgements
before granting further credit.

`file-complete` must match the active `fileId`, chunk count, and byte count.
Only successful SHA-256 verification, file synchronization, and atomic commit
produce `file-received`, including the actual saved filename. `file-error`
reports rejection; `file-cancel` produces `file-cancelled` and removes partial
data. Empty files still require `file-start` and `file-complete`.

The current browser limit is **512 MiB per file**. The device transport/storage
limit is **4 GiB per file**; the larger device limit does not override the
browser's memory-conscious cap. Only one file and one browser session are
accepted at a time.

The helper never chooses a filesystem destination. Its inherited Unix
`socketpair` uses a four-byte big-endian frame length, then a one-byte command
and payload. Maximum frame length is 1 MiB + 9 bytes:

| Command | Payload |
| --- | --- |
| `P` request approval | uint64 request ID, uint64 size, UTF-8 filename |
| `Q` query approval | uint64 request ID |
| `B` begin | uint64 request ID, uint64 size, 64 ASCII SHA-256 bytes, UTF-8 filename |
| `D` data | uint64 sequence followed by chunk bytes |
| `F` finish | Empty |
| `C` cancel | Empty |
| `S` status | At most 1,024 UTF-8 bytes |
| `R` response | uint8 success (`0` or `1`), then UTF-8 text |

Integers are big endian. Every command gets one `R` response before the next
command is sent. `P` immediately returns `pending`; `Q` returns `pending` or
`approved`, or fails with a rejection or timeout. The transport polls `Q` every
50 ms without waiting for a human inside an IPC request. A successful `F`
returns the saved filename; other successful responses have empty text. A
failure returns its error text. Cancellation
preserves request/response ordering so a late data response cannot be mistaken
for the cancellation response.

The receiver binds approval to one request ID, filename, and size. `B` consumes
that approval once and rejects a mismatched request. Cancelling or closing the
session revokes pending approval as well as incomplete storage.

## Filesystem boundary

The destination is the authenticated user's `$HOME/Downloads`, never the root
service's inherited `HOME`. Launcher opens path components without following
symlinks, verifies the home owner and write permissions, and creates or verifies
`Downloads` with mode `0700`. Completed files belong to that user with mode
`0600`.

The C++ `Transfer` writes temporary files in the same directory, validates
sequence and total length, and computes SHA-256 while writing. Only a matching
digest permits atomic publication after `fsync`. The browser path always uses
numbered auto-renaming for conflicts, such as `report (1).pdf`; existing files
are preserved. Long names are shortened at UTF-8 boundaries without overflowing
the 255-byte filename budget.

Filenames must be one component. Absolute paths, separators, dot names, control
characters, and traversal are rejected. Existing symlinks are never followed
or overwritten. Cancellation, protocol failure, and connection loss remove the
temporary file. The lower-level storage API also supports fail/replace policies
for other trusted adapters; the browser adapter does not request replacement.

## Validation

The host checks cover storage safety, IPC framing and process cleanup,
Bluetooth framing, automatic connection/file-confirmation protocol, and the
file name/size plus Accept/Decline/Close layout
at 568×1232 and 320×240. Native libdatachannel checks create two real local
peers. These checks do not by themselves prove a browser-to-board
Bluetooth radio transfer. Use the opt-in target harness and the browser flow
above for that validation; commands and coverage are in
[tests/README.md](../tests/README.md#browser-lan-file-sharing).

The C++ migration was validated on 2026-10-20: all 31 host CTests passed,
including the native receiver and private D-Bus tests. The ARM64 board also
passed native framing/IPC/pairing and real WebRTC peer transfers, plus the
private mock BlueZ tests. A five-second harness run registered GATT and
advertising with the board's real BlueZ daemon and exited cleanly. This did
not include a browser-to-board Bluetooth file-sharing session. The staged
transport install contained the native helper and dependency notices, with
no Python files or embedded SDK library search paths.


Validation on 2026-10-21 covered automatic connection and per-file confirmation:
the browser unit tests and production build passed, as did native storage,
receiver, WebRTC, BlueZ, and compact UI checks. A temporary C++ helper on the
board established a Chrome-to-board local DataChannel without code entry;
a file request reached the pending confirmation state and was cancelled.
The receiver did not save that file. This did not test accepting a file through
the board's production Launcher UI, which was not replaced for the check.
The successful captured connection had 58 ATT Write Requests and 58 Write
Responses. The earlier intermittent GATT write error was not reproduced; this check does not establish that it has been eliminated.
