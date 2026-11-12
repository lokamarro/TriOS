# Private Virtio-GPU memory-report contract

`virtio_gpu_uml.h` owns the packed little-endian wire of one private extension
of the Virtio-GPU device: the host tells the guest kernel how near the device
memory budget is and which guest processes hold it. It is independent of the
public API and the host/kernel bridge ABI, and the wire is the same for every
maintained kernel slice.

The device offers `VIRTIO_GPU_F_UML_MEMORY_REPORT` (bit 23) only while its
renderer has a memory budget. With the feature there is one query,
`VIRTIO_GPU_CMD_UML_GET_MEMORY`, and one event, `VIRTIO_GPU_EVENT_UML_MEMORY`
(`events_read` bit 1, cleared through `events_clear`):

- A query answers at once from the host's counters: the budget, the pool's use,
  what the guest's contexts hold, the refusal count, a state, and up to 16
  holders by guest process (largest first, filtered by size and idle time). The
  reply is cut to the writable descriptors the guest gave and never waits for
  the renderer.
- `SUBSCRIBE` makes the host raise the event when its pool goes under or out of
  pressure and when an allocation is refused. A query without it ends the
  subscription. The state is under pressure once the pool's use and a 96 MiB
  margin have reached the point the host refuses at, 16 MiB inside the budget,
  for a second after the host has freed what it caches itself, so the pressure
  comes before the host refuses the guest, and calm again after five calm
  seconds; a refusal raises the event at most once a second. A calm guest sees
  no traffic.
- `GUEST_RECLAIMS` records that a reclaimer in the guest acts on the reports,
  and the host's own reclaimer stands down meanwhile. It lasts for the query
  that set it and the connection.

Edit the canonical header, then use `sync-protocol.py --kernel-source PATH` to
copy it to `drivers/gpu/drm/virtio/virtgpu_uml.h` in each patch-producing
kernel checkout, amend the owning topic and regenerate each line's series;
`--verify` is the ordinary check. The tool also refuses a kernel whose
`virtio_gpu.h` already uses one of the header's feature, event, command or
response codes. Never edit generated patch output. A change to the driver runs
`guest-virtgpu-memory-report` before a kernel release. The
[kernel README](../README.md#devices) describes the guest file.
