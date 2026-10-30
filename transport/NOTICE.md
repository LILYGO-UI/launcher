# Native file-sharing dependencies

The private C++ file-sharing helper statically links unmodified libdatachannel
0.23.2 and its required source submodules. GDBus/GLib and OpenSSL are dynamically
linked system libraries. No Python transport is installed.

The exact libdatachannel source is available at
https://github.com/paullouisageneau/libdatachannel/tree/9e6a13abbb6846c003d817d0387b6706466e2b03
under Mozilla Public License 2.0. That tree pins these dependencies:

| Dependency | Source revision | License |
| --- | --- | --- |
| libjuice | `b509b6c1f90e4a1d4753e6bde62ffbeb4d10c6bb` | Mozilla Public License 2.0 |
| usrsctp | `fec583d54493f879d2ae44a743423bf8a04371ab` | BSD 3-Clause |
| plog | `94899e0b926ac1b0f4750bfbd495167b4a6ae9ef` | MIT |
| nlohmann JSON | `55f93686c01528224f448c19128836e7df245f72` | MIT |

Initialize the submodules recorded by that commit to obtain the corresponding
source. Each dependency's license and copyright notice accompanies the binary
under `/usr/share/doc/lilygo-ui-launcher/transport/<dependency>/`. Media and
WebSocket support are disabled; libSRTP is not linked into this helper.
