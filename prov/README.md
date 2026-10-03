# libfabric provider "uet"

`libuet-fi.so` is a libfabric DL provider named `uet`. It runs the
reference provider's SES/PDS/TSS layers and its raw socket NIC shim
inside an application that uses only the public libfabric API. It
supports what an RMA write consumer needs: RDM endpoints, RMA write as
initiator and remote write as target, and manual progress.

The core is compiled into the library with the vendored libfabric 1.20
headers, as for `libuet_fabric.so`. The glue in this directory is
compiled with the installed libfabric headers (tested with Fedora's
libfabric 2.3.1). The library exports only `fi_prov_ini` and links
`-lfabric`.

## Build and run

    make prov                  # or: make -C prov
    FI_PROVIDER_PATH=$PWD/prov fi_info -p uet -v

No `LIBFABRIC` build tree is needed. An application links with
`-lfabric` only and finds the provider through `FI_PROVIDER_PATH`.
`fi_getinfo` needs no privileges. Opening a domain opens a raw socket,
which needs `CAP_NET_RAW`, for example:

    sudo setpriv --reuid=$(id -u) --regid=$(id -g) --init-groups \
        --inh-caps=+net_raw --ambient-caps=+net_raw -- \
        env FI_PROVIDER_PATH=$PWD/prov UET_IFNAME=eth1 ./app

`prov/run_tests.sh all` creates network namespaces, runs the scenarios
below with `prov/test_rma`, and removes the namespaces again.

## Over a rocm-ernic engine (CORE=ernic)

The same glue can run over `libuet_ernic` instead of the reference
core. `libuet_ernic` is part of rocm-ernic, an emulated AMD Pensando
ionic NIC that runs the reference provider's SES/PDS/TSS as device
firmware (its `--uet` option). In a guest with such a device, the
library drives the engine through an RC queue pair on the ionic
device, so the provider needs no raw socket and no privilege.

    make -C prov ernic ERNIC=/path/to/rocm-ernic   # ernic/libuet-fi.so
    FI_PROVIDER_PATH=$PWD/prov/ernic fi_info -p uet

`ERNIC` defaults to `../../rocm-ernic`. `libuet_ernic.c` is compiled
into the library, which links `-lfabric -libverbs`. `test_rma` uses
only libfabric and serves both builds.

What differs from the reference core:

* A domain is an ibverbs device, not a netdev: `hints->domain_attr->name`,
  `FI_UET_IFNAME` or `UET_ERNIC_DEVICE`, else one `fi_info` for every
  ionic device whose engine answers. The address is the engine's, not
  one of the guest's.
* `cq_data_size` is 0, and `fi_writedata()` returns `-FI_ENOSYS`: the
  engine raises no events at the target. A target checks its window.
* `fi_close()` cannot discard writes already handed to the device
  (`uet_ep_abort()` returns `-FI_ENOSYS`), so it drops the queued ones
  and waits up to 10 s for the rest, as with `UET_PDS=sng`.
* Writes go in segments of 1 MiB, 4 in flight: each segment is a
  command to the device, and the device paces RUDI itself.
* `FI_UET_TX_TIMEOUT` and `FI_UET_TX_RETRIES` do not apply; the
  engine's retransmit settings are the device's (`--uet rto=,retries=`).

## fi_getinfo

Accepted hints: provider `uet`, `FI_EP_RDM`, caps from
`FI_RMA | FI_WRITE | FI_REMOTE_WRITE | FI_REMOTE_COMM`, any mode bits
(the provider requires none), `mr_mode` including `FI_MR_PROV_KEY`,
any threading level, manual progress, `FI_AV_TABLE` or `FI_AV_MAP`.

The provider is `FI_THREAD_SAFE`, and reports the level the hints ask
for (`FI_THREAD_SAFE` without one). Every call on a domain, or on an
object of it, runs under the domain's lock, except the slow part of
`fi_av_insert()`: resolving a new peer's next hop, which can wait on
ARP, runs without it, so other threads write and read completions
meanwhile.

The netdev is `hints->domain_attr->name` if set, else `FI_UET_IFNAME`,
else `UET_IFNAME`, else one `fi_info` for every interface that is up,
Ethernet, and has an IPv4 address. The domain name is the netdev name.
The fabric name is `uet`.

## Addresses

`fi_getname()` returns 32 bytes (`addr_format` `FI_FORMAT_UNSPEC`).
The format is self-contained and byte-order independent, so it can be
passed between hosts as is. `fi_av_insert()` takes the same bytes.

| bytes | content |
|-------|---------|
| 0     | `0x55` |
| 1     | format version, 1 |
| 2     | UET address flags (`UET_ADDR_*`) |
| 3     | 0 |
| 4-5   | FEP capabilities, big endian (the HPC profile is set) |
| 6-7   | PIDonFEP, big endian |
| 8-9   | resource index, big endian |
| 10-11 | number of indices, big endian |
| 12-15 | initiator id, big endian |
| 16-31 | IPv4 address in network order in bytes 16-19, then zeros |

The first time a peer is inserted, the core resolves its next hop over
rtnetlink: the route out of the interface (inside its VRF, if it is in
one), then the neighbor table, and prints the result to stdout. Without
a usable neighbor entry, a datagram to the next hop makes the kernel
resolve it by ARP, and the table is polled for up to `UET_NH_WAIT_MS`
(default 1000) milliseconds. With `UET_NH_WAIT_MS=0` the insert returns
at once and a write gets `-FI_EAGAIN` until the peer has answered. A
peer that never answers fails the write with `-FI_ENETUNREACH`. Later
insertions of the same address reuse that entry.

## Memory registration

`mr_mode` is `FI_MR_PROV_KEY` only:

* Keys are chosen by the provider and are 64 bits (`mr_key_size` 8).
  The key is the UET memory key: bit 63 `IDEMPOTENT_SAFE`, bit 48 marks
  a provider-assigned key, and the low bits are the region index.
* Remote addresses are offsets into the region. `FI_MR_VIRT_ADDR` is
  not set, so a writer addresses byte `n` of a window as `n`. A consumer
  that computes the remote address as token base + offset must use base
  0 for this provider.
* `FI_MR_LOCAL` is not set. Local buffers need no registration, and
  `desc` is ignored.
* `FI_MR_ENDPOINT` is not required. The core binds a region to one
  endpoint. The provider binds every region of a domain to the domain's
  endpoint and enables it with the endpoint. `fi_mr_bind()` to the
  endpoint and `fi_mr_enable()` are accepted.

A region registered with `FI_REMOTE_WRITE` is marked `IDEMPOTENT_SAFE`
(set `FI_UET_RUDI=0` to turn this off). Writes to it then use RUDI,
which keeps no state at the target. A RUDI packet can be placed more
than once, because it is retransmitted until a response arrives. A
delayed duplicate can therefore rewrite bytes after the write that
carried them has completed. That matters only if someone else rewrites
the same bytes in the meantime.

## Writes and completions

`fi_write`, `fi_writev` (one iov), `fi_writemsg` (one iov and one
rma_iov) and `fi_writedata` are supported. Inject, read, send, tagged,
atomic and collective operations return `-FI_ENOSYS`.

* Completion semantics: a write completes when every packet of it has
  been answered by the target. The target answers a packet only after
  copying its payload into the region. A completion therefore means the
  data is in target memory. This is `FI_DELIVERY_COMPLETE`, and it is
  also what `FI_TRANSMIT_COMPLETE` and `FI_INJECT_COMPLETE` give.
* Targets keep no state for initiators. A target does not need its
  writers in its AV. Writes without immediate data go over RUDI.
  `fi_writedata` goes over RUD, which also needs no AV entry. The
  immediate data is delivered on the target's CQ (bound with
  `FI_RECV`, format `FI_CQ_FORMAT_DATA`) with flags
  `FI_RMA | FI_REMOTE_WRITE | FI_REMOTE_CQ_DATA`, after the whole write
  has landed.
* Progress is manual. `fi_cq_read` makes progress on the endpoints
  bound to the CQ: sending and retransmitting on an initiator, placing
  incoming writes on a target. A target must read its CQ in a loop. It
  must make progress at least once per retransmit timeout times retry
  count (200 ms x 25 = 5 s by default), or writes to it fail with
  `FI_EIO`.
* The core has no flow control for RUDI. It sends all packets of a
  message at once, so a burst larger than the receiver's socket buffer
  is lost and recovered only by 200 ms timeouts. The provider therefore
  sends a write as segments of 16 packets (16 KiB at an MTU of 1500,
  128 KiB at 9000) with at most 2 in flight per endpoint. Writes of any
  size are accepted; up to `tx_attr->size` writes can be outstanding.
* The core carries UET in UDP to port 4793 by default (`FI_UET_ENCAP=ip`
  puts it directly in IP protocol 253, behind an entropy header), and
  takes packets in either form. Its Payload MTU is the largest of 1024,
  2048, 4096 and 8192 bytes that fits the netdev's MTU, unless
  `FI_UET_MAX_PAYLOAD` sets it. Every peer must use the same Payload
  MTU, so give peers' interfaces the same MTU.
* `fi_close()` on an endpoint discards the writes it still has
  outstanding, as fi_endpoint(3) requires: no completion is reported for
  them, and none of their packets is sent or retransmitted once the
  close has started. The close takes microseconds. The same process can
  open a new endpoint on the domain at once; it gets the same address,
  and registered regions are bound to it. A RUD PDC that loses packets
  this way is closed with the peer by a CLOSE control packet, which
  carries no data, or freed if it was never established. (With
  `UET_PDS=sng`, which cannot discard, the close waits up to 10 s for
  writes in flight instead.)
* Errors are reported through `fi_cq_readerr`. Transport errors such as
  an unknown key, an out-of-range offset or exhausted retries all
  arrive as `FI_EIO`, because the core does not pass the SES return
  code up. `fi_cq_strerror` prints the error.
* CQ formats CONTEXT, MSG, DATA and TAGGED are supported. There are no
  wait objects, so `fi_cq_sread` returns `-FI_ENOSYS`.

## Parameters

| variable | default | meaning |
|----------|---------|---------|
| `FI_UET_IFNAME` | `$UET_IFNAME` (`$UET_ERNIC_DEVICE` for CORE=ernic) | netdev (ibverbs device for CORE=ernic) when the hints do not name one |
| `FI_UET_RUDI` | 1 | mark remotely writable regions `IDEMPOTENT_SAFE` and set `UET_FORCE_RUDI` |
| `FI_UET_TX_TIMEOUT` | `$UET_PDS_TX_TIMEOUT`, else 200 | retransmit timeout in ms |
| `FI_UET_TX_RETRIES` | `$UET_PDS_MAX_TX_RETRIES`, else 25 | retransmissions before a write fails |
| `FI_UET_SEGMENT_SIZE` | 16 packets of the Payload MTU (1048576 for CORE=ernic) | bytes per write segment |
| `FI_UET_MAX_SEGMENTS` | 2 (4 for CORE=ernic) | segments in flight per endpoint |
| `FI_UET_PROGRESS_BURST` | 64 | core progress calls per CQ read; each handles at most one received packet |
| `FI_UET_ENCAP` | `$UET_ENCAP`, else `udp` | `udp` (UDP port 4793) or `ip` (IP protocol 253); exported as `UET_ENCAP`. CORE=ernic: the engine's `encap=` decides |
| `FI_UET_MAX_PAYLOAD` | `$UET_MAX_PAYLOAD`, else from the MTU | Payload MTU, 1024, 2048, 4096 or 8192; exported as `UET_MAX_PAYLOAD`. CORE=ernic: the engine's `mtu=` decides |

When a domain is first opened, the provider sets `UET_PDS=pds` unless
it is already set. The core reads the `UET_*` variables, so they can
still be used directly.

## Limits

* One netdev and one endpoint per process at a time. The core keeps
  its packet delivery state in globals, and every endpoint gets the
  default resource index. Several processes on one host need separate
  interfaces and IP addresses: the raw socket receives every UET packet
  on its interface.
* IPv4 only.
* The core logs to stdout. Its raw socket also sees outgoing and
  non-UET frames, and the PDS warns about each of them.
* Throughput between namespaces on veth: about 250 MiB/s for one
  writer, and about 140 MiB/s each for two writers into one target.
  More concurrent writers into one target cause packet loss, which the
  200 ms timeouts recover from slowly.
