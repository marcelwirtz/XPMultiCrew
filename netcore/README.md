# netcore

Network client library: state serialization, UDP/QUIC transport, NAT
traversal client side. See `docs/plan.md`, sections 3, 7 and 11.

`include/flytogether/` currently holds the Phase 0 spike only: a tiny
cross-platform blocking UDP wrapper (`udp_socket.h`) and the position wire
struct (`position_packet.h`) used to prove the plugin can get a dataref to a
second local process over a real socket. This gets replaced by the real
transport (QUIC or ENet, reliable+unreliable channels, ideally with
compression/encryption) starting Phase 1/2 - see the tech stack table in
`docs/plan.md` section 8 for the two options under consideration (C++ in the
plugin vs. a separate Rust process/library over local IPC).
