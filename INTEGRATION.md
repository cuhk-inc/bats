# Integrating the BATS Codec

This document describes how to use the BATS codec inside an existing transport. The library provides encoding, recoding, and decoding only. The caller retains sockets, headers, scheduling, rate control, and loss. Do not link `link.c`, `relay.c`, `sim.c`, or `main.c`, and do not substitute `bats_sim` or `bats_line` for the caller's own forwarding path.

The library exposes three operations. The source calls `bats_encode`. Each relay calls `bats_recode`. The destination calls `bats_decode`. Every batch produces `BATS_M` packets, which is 8. The outer code draws a degree from the distribution Ψ, selects that many source packets, and forms the matrix `G`. A relay mixes only the packets of one batch that have already arrived. Neither `G` nor the selected source indices are placed in the packet.

## Build

```bash
make lib
```

The archive is `libbats.a`. The caller includes only `bats_codec.h`:

```bash
gcc -std=c11 -I/path/to/bats her_program.c /path/to/bats/libbats.a -o her_program
```

Call `bats_init` once in the process, before the first encode, recode, or decode.

## Parameters agreed in advance

The source and the destination must use the same parameters. A relay does not read them.

| Parameter | Used by | Agreement |
| --- | --- | --- |
| `K` | Source, destination | Number of source packets in one generation |
| `T` | Source, destination, relay | Payload length of one packet, in bytes |
| `code_seed` | Source, destination | One shared 64-bit integer |
| Ψ | Source, destination | Leave `psi_set` unset at both ends, or pass the same weights to both |
| `batch_id` | Written by the source, forwarded unchanged, read by the destination | Starts at 0 within each generation and increases by one |

When the object length is not a multiple of `T`, the source pads the last source packet with zeros before encoding. After a successful decode, the destination discards that padding.

`bats_decode` returns 1 when the equations it constructed have a solution. A mismatched Ψ or `code_seed` still produces a solved buffer. A checksum over the object belongs in the caller.

## Fields carried in each packet

The library does not construct a header. Each packet must carry at least the following fields.

| Field | Size | Origin |
| --- | --- | --- |
| `batch_id` | 4 bytes | The identifier passed to `bats_encode` |
| Coefficient | 8 bytes | `coeff[j * 8]` at the source. After recoding, the coefficient produced by `bats_recode` |
| Payload | `T` bytes | `payload[j * T]` at the source. After recoding, the payload produced by `bats_recode` |

The coefficient must be forwarded after every relay. Without it, the destination cannot form equations.

The same `code_seed` and `batch_id` always regenerate the same batch. Transmitting that `batch_id` again repeats the same eight packets. A new batch, and therefore new equations, requires a new `batch_id`.

## Source

Lay the generation out as `K * T` contiguous bytes. Begin at `batch_id` 0 and call `bats_encode` once per batch, increasing `batch_id` by one each time. Each call writes eight packets.

| Buffer | Size | Layout |
| --- | --- | --- |
| `coeff` | 64 bytes | Packet `j` starts at `coeff[j * 8]` |
| `payload` | `8 * T` bytes | Packet `j` starts at `payload[j * T]` |

The call returns 0 on success and `-1` when an argument is invalid.

One call draws a degree `d` from Ψ, using `K` when `d` exceeds `K`, selects `d` source packets uniformly, and fills a `d × 8` matrix `G`. The eight payloads are those source packets multiplied by `G`. The coefficient of source packet `j` is column `j` of the `8 × 8` identity.

One batch covers only `d` of the `K` source packets. For a large `K`, continue with `batch_id + 1`. The destination calls `bats_decode` on every packet received so far and stops when the call returns 1. Further batches are required when the path loses packets.

```c
#include "bats_codec.h"

enum { K = 128, T = 1024 };

int emit_one_batch(const uint8_t src[K * T], uint64_t code_seed, uint32_t batch_id,
                   uint8_t coeff[BATS_M * BATS_M], uint8_t payload[BATS_M * T]) {
    return bats_encode(src, K, T, code_seed, batch_id, coeff, payload);
}
```

## Relay

A relay does not call `bats_encode`, `bats_decode`, or `psi_set`. It calls only `bats_recode`.

1. Store arrivals in a bucket keyed by `batch_id`. Packets from two batch identifiers must not be passed to one `bats_recode` call. They were formed from different subsets of the source.
2. Keep one `uint64_t recode_state` for each hop of each flow. Set it to a nonzero seed before the first recode on that hop. Pass the same variable on every later call. The library updates it. Give every other hop, and every other flow, its own state.
3. Recode and forward as soon as all eight packets of the batch have arrived. If the batch is still incomplete when the hop becomes idle, and at least one packet is present, recode the packets on hand. If the bucket is empty, forward nothing. A call with `n_in == 0` writes no packets.
4. Drop a repeated packet of the same batch. Pass only packets with distinct coefficients.
5. Pack the input in packet order. Packet `s` occupies `coeff_in[s * 8]` and `payload_in[s * T]`. When `n_in > 0`, the call writes eight new packets and sets `*n_out` to 8. Forward each new coefficient with its new payload. Do not reuse the coefficient that arrived.
6. The input and output buffers may overlap. An invalid argument returns `-1`.

The destination does not recode.

```c
uint64_t recode_state = 11; /* Initial value for this hop and this flow. */

int relay_one_batch(const uint8_t *coeff_in, const uint8_t *payload_in, int n_in, int T,
                    uint8_t *coeff_out, uint8_t *payload_out) {
    int n_out = 0;

    return bats_recode(coeff_in, payload_in, n_in, T, &recode_state,
                       coeff_out, payload_out, &n_out);
}
```

When `bats_recode` returns 0 and `n_out` is 8, transmit those eight packets. When `n_in` is 0, `n_out` is 0.

For several flows, the header must distinguish them. Each flow may number `batch_id` from 0. Each flow keeps a separate `recode_state` on each hop.

## Destination

The destination does not recode. Assemble the received packets into a `BatsSymbol` array. Copy `batch_id` and the eight coefficient bytes into the symbol, and point `payload` at `T` live bytes. Symbol order does not matter.

```c
int n_bp = 0;
int n_inact = 0;
int ok = bats_decode(symbols, n_symbols, K, T, code_seed, dst, &n_bp, &n_inact);
```

| Return | Meaning |
| --- | --- |
| 1 | `dst` contains all `K` source packets |
| 0 | The equations are insufficient, or `n_symbols == 0` |
| -1 | An argument is invalid, or a symbol has a null `payload` |

Each `payload` pointer must remain valid until `bats_decode` returns. `n_bp` and `n_inact` may be `NULL`. After a return of 1, remove the padding on the final packet and check the caller's checksum.

## Degree distribution on a new network

If neither end calls `psi_set`, both use the built-in distribution. Its degrees are 1 through 8, and its weights are 12, 30, 40, 44, 40, 36, 30, and 24. Use this distribution for the first measurement on a new path.

Ψ is fitted to the rank of each batch at the destination, not to a loss percentage. The rank is the rank over GF(256) of the coefficient matrix that remains for that batch. Place each received 8-byte coefficient in a row of an `r × 8` matrix and row-reduce it. The number of nonzero rows is the rank. A batch from which nothing arrives has rank 0. The number of packets is not the rank: after recoding, additional packets may be linearly dependent.

Obtain the distribution as follows.

1. Run the source, relays, and destination over the real path, using the procedure above. Collect several hundred batches before treating the histogram as stable. Both ends use the same Ψ for the whole measurement. On a first measurement, that is the built-in distribution. Do not change Ψ while the measurement is in progress.
2. At the destination, count how many batches have each rank from 0 through 8. The result is nine integers, `h0` through `h8`. A `batch_id` that the source finished sending, and from which nothing arrived, contributes to `h0`. Do not count a batch the source has not yet sent.
3. Build the optimizer in the `bats` directory and pass those nine integers. The degree cap must not exceed `K`. A larger degree is reduced to `K` during encoding.

```bash
make psi_opt
./psi_opt --eta-bar 0.99 --max-degree 128 --weights-out psi_network.txt \
    h0 h1 h2 h3 h4 h5 h6 h7 h8
```

`--eta-bar` is the fraction of source packets that belief propagation is required to sustain. The default is 0.99. The optimizer writes `psi_network.txt` within a few seconds. The first value in the file is the maximum degree `D`. The following line contains the integer weights of degrees 1 through `D`. Lines beginning with `#` are comments.

4. Install that file at both the source and the destination. After `bats_init`, and before the first packet, both ends load the same weights. A relay does not load them.

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

`psi_set` returns 1 on success. It stores one distribution for the process, so it must be called before any encode or decode in that process.

5. Use the codec again on the next transfer. The selected packets and `G` agree only when the source and the destination have both installed `psi_network.txt`. Updating one end alone makes the destination solve for the wrong packets, and `bats_decode` may still return 1.

When the path, the number of relays, or the loss changes materially, repeat the rank measurement with the distribution then in use, run `psi_opt` again, and replace the file at both ends together. Do not change Ψ in the middle of a transfer.
