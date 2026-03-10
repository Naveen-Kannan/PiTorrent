# PiTorrent

## What the project is

We built the underlying system for BitTorrent-style peer-to-peer file sharing on Raspberry Pis. That means the Bluetooth side (driving the chip and forming connections), the way Pis find and talk to each other, and the application protocol on top. Pis run a single binary and talk over Bluetooth Classic. The system handles discovery (both Bluetooth inquiry and gossip), forms and manages multiple connections to other Pis, exchanges info about which chunks each peer has, and transfers chunks between direct peers with integrity checks. A small distributed lookup (DHT) and message forwarding let control traffic reach Pis that aren't directly connected.

## The idea

The goal was to get some of the core BitTorrent ideas (peer discovery, chunk exchange, integrity checking) working in a minimal setting. We wanted a partial mesh (each Pi only has to connect to a few peers and not everyone), discovery that doesn't assume a pre-wired full mesh, and a way to route control traffic across the overlay when not all nodes are directly connected. So the design uses Bluetooth underneath, then a small custom protocol (our own packet format with message types for requests, chunks, bitfield, gossip, and DHT). Direct peers exchange bitfields so they know what chunks each has. When we need to send a message to a Pi we're not directly connected to, we forward it through neighbors so it can cross the mesh. Chunk data itself is only sent between direct peers, as in real BitTorrent.

## Challenges

**From GPIO to Bluetooth.** We first built a version that ran over the GPIO pins. It worked but was tied to physical wiring and didn't scale as nicely. Moving to Bluetooth was more elegant (no cables between Pis), and we could use the standard interface (HCI) to drive the chip. The main challenge was implementing the Bluetooth. The Pi's BCM43430 chip needs firmware loaded over the PL011 UART before it can do discovery, form connections, or send data. So we had to get the PL011 UART right, implement HCI (commands, events, and data packets), and load the vendor firmware. We completed the 340LX lab for this.

**Multiple connections.** There's one PL011 UART to the chip, but we can have several connections. Each packet from the chip says which connection it belongs to, so we map that to our own per-connection queues. That way we can receive from several Pis "at once" (interleaved) and know which peer each packet came from.

**Gossip and partial mesh.** We didn't want to require a full mesh from the start (every Pi connected to every other). So we added gossip. Peers periodically exchange lists of peers they know about. That way a Pi can learn about nodes it isn't directly connected to and, if it has free capacity, open a new connection to them. Discovery is a mix of Bluetooth inquiry (find nearby Pis) and gossip (learn about peers from connected neighbors). For control traffic (e.g. DHT) to nodes we're not directly connected to, we forward the message by wrapping the message in a "forward" envelope with a destination and TTL and send it to a neighbor, who either delivers it locally or forwards it again.

## Devices

- **Raspberry Pis** running our bare-metal binary. Each has an on-board BCM43430 Bluetooth chip connected to the CPU via the PL011 UART (GPIO 30-33). We use GPIO 45 to enable the Bluetooth chip. We use the course's libpi and a small stack (PL011 UART driver, HCI, our network and app layers).
- **Host machines** for building and deploying the binary to each Pi over USB-serial; optionally used to drive tests (e.g. sending initial chunk data over serial). Same serial link is used for debug output.

## Errata
