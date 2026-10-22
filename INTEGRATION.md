# Using BATS

This guide explains how to use this BATS codec to send an object across a network. Read it from the top. Each section is the next thing the caller does.

The library encodes, recodes, and decodes. The caller keeps its own sockets, headers, scheduling, rate control, and loss. Link `libbats.a`, built from `field.c`, `source.c`, `dest.c`, and `codec.c`, and include only `bats_codec.h`. Leave `link.c`, `relay.c`, `sim.c`, `main.c`, and `psi_opt.c` out of the caller's program. `psi_opt` is a separate tool, used once between transfers when a new network needs its own degree distribution.

```bash
make lib
gcc -std=c11 -I/path/to/bats caller.c /path/to/bats/libbats.a -o caller
```

Call `bats_init` once in each process, before the first encode, recode, or decode.

## What a transfer does

An object is split into generations of `K` packets, each packet `T` bytes. One generation is one use of the codec. Inside a generation the source sends batches. Every batch is eight packets. The eight packets are a mixture of `d` source packets, where `d` is drawn from the degree distribution Ψ. The mixture matrix `G`, and the identities of those `d` packets, are not written into the packet. The destination rebuilds both from the shared `code_seed` and the `batch_id`.

A relay does not know Ψ or `G`. It mixes the packets of one `batch_id` that have already arrived, and forwards eight new packets. The destination collects packets from every `batch_id`, rebuilds each batch, and solves the generation.

The source, every relay, and the destination each call one function:

| Node | Call | When |
| --- | --- | --- |
| Source | `bats_encode` | Once per batch, with the next `batch_id` |
| Relay | `bats_recode` | When a batch is ready to forward |
| Destination | `bats_decode` | As packets of the generation arrive |

## Agree the parameters

The source and the destination use the same values for one generation. A relay does not read `K`, `code_seed`, or Ψ.

| Parameter | Meaning |
| --- | --- |
| `K` | Number of source packets in the generation |
| `T` | Payload length of one packet, in bytes |
| `code_seed` | Shared 64-bit integer. With `batch_id`, it rebuilds `G` |
| Ψ | Degree distribution. Leave `psi_set` unset and both ends use the built-in weights. After a network measurement, both ends load the same file |
| `batch_id` | Starts at 0 in the generation and increases by one. The same `code_seed` and `batch_id` always produce the same eight packets |

On a network that has not been measured yet, use the built-in Ψ. Its degrees are 1 through 8 and its weights are 12, 30, 40, 44, 40, 36, 30, 24. The section "A degree distribution for a new network" replaces it later. Use one distribution for a whole transfer.

If the object is longer than `K * T` bytes, send it as successive generations. Number generations from 0. Start `batch_id` again at 0 in each generation. If the object length is not a multiple of `T`, pad the last source packet with zeros. The destination removes that padding after decoding.

Put a checksum over the original object, before padding, in the caller's own header or side message. `bats_decode` returns 1 when the equations it built have a solution. Those bytes are the object only when both ends used the same `K`, `T`, `code_seed`, and Ψ. The checksum is how the caller knows.

## What every packet carries

The library does not build a header. Add the following fields to the caller's packet, and forward them at every hop.

| Field | Size | Written by |
| --- | --- | --- |
| Flow identifier | 2 bytes | The caller. Use 0 while one flow is enough |
| Generation | 4 bytes | The caller. Use 0 while the object fits in one generation |
| `batch_id` | 4 bytes | The value passed to `bats_encode` |
| Coefficient | 8 bytes | `coeff[j * 8]` at the source. After a relay, the coefficient returned by `bats_recode` |
| Payload | `T` bytes | `payload[j * T]` at the source. After a relay, the payload returned by `bats_recode` |

The destination uses the coefficient to form its equations. The relay forwards the coefficient it just computed, together with the new payload.

The library does not tell the source to stop. After `bats_decode` returns 1 and the checksum matches, the destination sends the caller's completion signal. The source then stops that generation.

## Source

Lay the generation out as `K * T` contiguous bytes. Start at `batch_id` 0. Call `bats_encode` once per batch and give the eight packets to the existing sender. Increase `batch_id` by one and repeat until the completion signal arrives. Loss on the path means the generation needs further batches.

| Buffer | Size | Layout |
| --- | --- | --- |
| `coeff` | 64 bytes | Packet `j` starts at `coeff[j * 8]` |
| `payload` | `8 * T` bytes | Packet `j` starts at `payload[j * T]` |

The call returns 0 on success and `-1` when an argument is invalid.

```c
#include "bats_codec.h"

int emit_one_batch(const uint8_t *src, int K, int T, uint64_t code_seed,
                   uint32_t batch_id, uint8_t *coeff, uint8_t *payload) {
    return bats_encode(src, K, T, code_seed, batch_id, coeff, payload);
}
```

One call draws a degree `d` from Ψ. When `d` is larger than `K`, the call uses `K`. It selects `d` source packets uniformly and fills a `d × 8` matrix `G`. The eight payloads are those source packets multiplied by `G`. The coefficient of packet `j` is column `j` of the `8 × 8` identity. Sending the same `batch_id` again repeats those eight packets. A new batch needs a new `batch_id`.

## Relay

Each relay calls `bats_recode` and nothing else from this library. It does not load Ψ. The destination does not recode.

Configure the relay as follows.

1. Store arrivals in a bucket keyed by flow, generation, and `batch_id`. One call of `bats_recode` takes packets from one bucket only. Packets from two batches were formed from different subsets of the source.
2. Keep one `uint64_t recode_state` for each flow on that hop. Set it to a nonzero value before the first recode of the flow, and pass the same variable on every later batch. The library updates it. Give every other hop, and every other flow, its own variable. A new generation of the same flow keeps the same variable.
3. Drop a packet whose coefficient is already in the bucket. Pack the rest in arrival order. Packet `s` occupies `coeff_in[s * 8]` and `payload_in[s * T]`.
4. Recode and forward when the bucket holds eight packets. When the hop is idle and the bucket holds from one to seven packets, recode the packets on hand. An empty bucket forwards nothing. A call with `n_in == 0` writes no packets and sets `*n_out` to 0. Any other successful call writes eight packets and sets `*n_out` to 8.
5. Forward each new coefficient with its new payload. The input and output buffers may overlap. An invalid argument returns `-1`.

```c
uint64_t recode_state = 11; /* This hop, this flow, for the life of the flow. */

int relay_one_batch(const uint8_t *coeff_in, const uint8_t *payload_in, int n_in, int T,
                    uint8_t *coeff_out, uint8_t *payload_out) {
    int n_out = 0;

    return bats_recode(coeff_in, payload_in, n_in, T, &recode_state,
                       coeff_out, payload_out, &n_out);
}
```

Transmit the eight packets when the call returns 0 and `n_out` is 8.

## Destination

Append each arrival to a `BatsSymbol` array for that flow and generation. Copy `batch_id` and the eight coefficient bytes into the symbol, and point `payload` at `T` live bytes. Symbol order does not matter. Each `payload` pointer stays valid until `bats_decode` returns.

```c
int n_bp = 0;
int n_inact = 0;
int ok = bats_decode(symbols, n_symbols, K, T, code_seed, dst, &n_bp, &n_inact);
```

| Return | What the caller does |
| --- | --- |
| 1 | Remove the padding, compare the checksum, and send the completion signal when it matches |
| 0 | Wait for more packets. `n_symbols == 0` also returns 0 |
| -1 | An argument is invalid, or a symbol has a null `payload` |

`n_bp` and `n_inact` may be `NULL`. When they are not, they receive how many source packets belief propagation solved and how many inactivation solved. A return of 1 with a failed checksum means the two ends used different `K`, `T`, `code_seed`, or Ψ. Align those values and send the generation again.

A second flow uses a new flow identifier, its own `batch_id` sequence, its own symbol array, and its own `recode_state` on each hop.

## A degree distribution for a new network

Use the built-in Ψ for the first transfers on a path, with the source, relays, and destination configured as above. Ψ is then fitted to the ranks observed at the destination on that path. Rank is the rank over GF(256) of the coefficient rows that arrived for one `batch_id`. Place each received 8-byte coefficient in a row of an `r × 8` matrix and row-reduce it. The number of nonzero rows is the rank. Further packets can be linearly dependent, so the packet count is a different quantity. A batch the source finished sending, and from which nothing arrived, has rank 0. Leave out a batch the source has not sent.

Keep the same Ψ for the whole measurement. At the destination, count how many batches fall on each rank from 0 through 8. That is nine integers, `h0` through `h8`. Continue through further generations until the nine integers cover several hundred batches.

Build the optimizer in the `bats` directory. Cap the degree at `K` or below. Encoding reduces any larger degree to `K`.

```bash
make psi_opt
./psi_opt --eta-bar 0.99 --max-degree 128 --weights-out psi_network.txt \
    h0 h1 h2 h3 h4 h5 h6 h7 h8
```

`--eta-bar` is the fraction of source packets that belief propagation is required to sustain. The default is 0.99. The file is written within a few seconds. Its first value is the maximum degree `D`. The next line holds the integer weights of degrees 1 through `D`. Lines that begin with `#` are comments.

Copy `psi_network.txt` to the source and to the destination. After `bats_init`, and before the first packet of the next transfer, both ends load it. A relay does not load it. `psi_set` keeps one distribution for the process and returns 1 on success.

```c
int weights[4097];
int D;
int d;

weights[0] = 0;
/* After skipping comments, read D, then read weights[1] .. weights[D]. */
for (d = 1; d <= D; d++) {
    /* weights[d] = the integer read for degree d */
}
if (!psi_set(weights, D)) {
    /* Rejected: a weight is negative, or weights 1..D are all zero. */
}
```

Send the next object with this distribution at both ends. The checksum then matches. Loading the file at one end only makes the destination solve for the wrong packets, and `bats_decode` can still return 1.

Change Ψ only between transfers, and change it at both ends in the same step. When the path, the number of relays, or the loss changes, measure the ranks again with the distribution then in use, run `psi_opt`, and install the new file at both ends.
