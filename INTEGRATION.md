# Integrating the BATS Codec

This document is the order of work for placing the BATS codec into an existing network. Complete each stage and pass its check before starting the next. The library provides encoding, recoding, and decoding only. Sockets, headers, scheduling, rate control, and loss remain in the caller.

Link `libbats.a`, built from `field.c`, `source.c`, `dest.c`, and `codec.c`. Include only `bats_codec.h`. Leave `link.c`, `relay.c`, `sim.c`, `main.c`, and `psi_opt.c` out of the caller's program. `psi_opt` is a separate tool, run once between transfers.

Every batch emits `BATS_M` packets, which is 8. The outer code draws a degree from Ψ, selects that many source packets, and forms the matrix `G`. `G` and the selected indices are not placed in the packet. The destination rebuilds them from `code_seed` and `batch_id`. A relay mixes only packets that already share one `batch_id`.

## Order of work

1. Build the library and decode one generation in a single process, with no sockets.
2. Freeze `K`, `T`, `code_seed`, the packet layout, and the rule that stops the source. Use the built-in Ψ. Call no `psi_set`.
3. Carry those packets on the real path while every relay forwards them unchanged. The destination decodes and checks the object.
4. Enable `bats_recode` at one relay, check the object, then enable the next relay.
5. Accept the path only when the caller's checksum matches. `bats_decode` returning 1 is not that check.
6. On that same path, count the rank of several hundred batches and run `psi_opt`.
7. Install the resulting weights at the source and the destination together, then repeat the transfer. Leave the relays unchanged.

A later change of path, relay count, or loss returns to stage 6. It does not reopen stages 1 to 4.

## Stage 1. Library, one process

On the build machine:

```bash
make lib
gcc -std=c11 -I/path/to/bats bringup.c /path/to/bats/libbats.a -o bringup
```

`bringup` calls `bats_init` once, then `bats_encode` on `K * T` bytes with `batch_id` 0, fills one `BatsSymbol` per packet from the buffers below, and calls `bats_decode` with the same `K`, `T`, and `code_seed`. One batch is usually short of a full solution. Increment `batch_id` and repeat until `bats_decode` returns 1. Compare the recovered bytes with the original.

| Buffer | Size | Layout |
| --- | --- | --- |
| `coeff` | 64 bytes | Packet `j` starts at `coeff[j * 8]` |
| `payload` | `8 * T` bytes | Packet `j` starts at `payload[j * T]` |

`bats_encode` returns 0 on success and `-1` when an argument is invalid. `bats_decode` returns 1 when `dst` holds all `K` packets, 0 when the equations are short, and `-1` when an argument is invalid or a symbol has a null `payload`. Each `payload` pointer stays valid until `bats_decode` returns. Symbol order does not matter.

Check: the recovered bytes equal the original. Install this binary's archive on the source host, every relay host, and the destination host. Each of those processes calls `bats_init` once, before its first codec call.

## Stage 2. Parameters and packet layout

Write these down before changing the live forwarders. The source and the destination use the same values. A relay does not read `K`, `code_seed`, or Ψ.

| Parameter | Agreement |
| --- | --- |
| `K` | Number of source packets in one generation |
| `T` | Payload length of one packet, in bytes. Fit it to the caller's existing payload size |
| `code_seed` | One shared 64-bit integer for the whole trial |
| Ψ | Built-in. Degrees 1 through 8, weights 12, 30, 40, 44, 40, 36, 30, 24. Neither end calls `psi_set` |
| `batch_id` | Starts at 0 in each generation and increases by one. The same `code_seed` and `batch_id` always regenerate the same eight packets |

The object is split into generations of `K` packets. The last packet of the object is padded with zeros up to `T` bytes. A generation identifier starts at 0. `batch_id` starts again at 0 in the next generation. The first trial uses one flow and one generation. Further flows and further generations wait until stage 5 has passed.

Each packet already has whatever header the network uses. Add these fields and forward them at every hop.

| Field | Size | Set by |
| --- | --- | --- |
| Flow identifier | 2 bytes | 0 while the trial has one flow |
| Generation | 4 bytes | 0 while the object fits in one generation |
| `batch_id` | 4 bytes | The value passed to `bats_encode` |
| Coefficient | 8 bytes | `coeff[j * 8]` at the source. After a relay recodes, the coefficient from `bats_recode` |
| Payload | `T` bytes | `payload[j * T]` at the source. After a relay recodes, the payload from `bats_recode` |

The destination needs the coefficient. A relay that drops it leaves nothing from which to form equations.

Also choose how the source stops. The library has no reverse message. Two rules are used later:

- Transfer. The destination sends the caller's existing completion signal after `bats_decode` returns 1 and the checksum matches. The source then stops incrementing `batch_id` for that generation.
- Measurement, stage 6 only. The source keeps sending until the destination has counted several hundred finished batches. Decoding one object is not the end of that run.

Place a checksum over the original object bytes in the caller's header or in a side message. The checksum covers the object before padding.

Check: source, relays, and destination parse one captured packet into the same five fields, and the written parameters name one `K`, one `T`, and one `code_seed`.

## Stage 3. Real path, relays still forward

The source lays one generation out as `K * T` contiguous bytes. It starts at `batch_id` 0. Each call writes eight packets, which the existing sender transmits.

```c
#include "bats_codec.h"

int emit_one_batch(const uint8_t *src, int K, int T, uint64_t code_seed,
                   uint32_t batch_id, uint8_t *coeff, uint8_t *payload) {
    return bats_encode(src, K, T, code_seed, batch_id, coeff, payload);
}
```

One call draws a degree `d` from Ψ, using `K` when `d` exceeds `K`, selects `d` source packets uniformly, and fills a `d × 8` matrix `G`. The eight payloads are those source packets multiplied by `G`. The coefficient of source packet `j` is column `j` of the `8 × 8` identity. The next batch uses `batch_id + 1`.

During this stage a relay stores nothing and calls neither `bats_encode`, `bats_recode`, nor `bats_decode`. It forwards the five fields as they arrived.

The destination appends each arrival to a `BatsSymbol` array for that generation. It copies `batch_id` and the eight coefficient bytes, and points `payload` at `T` live bytes. After each arrival it calls:

```c
int n_bp = 0;
int n_inact = 0;
int ok = bats_decode(symbols, n_symbols, K, T, code_seed, dst, &n_bp, &n_inact);
```

`n_bp` and `n_inact` may be `NULL`. On 0, it waits for more packets. On 1, it removes the padding and compares the checksum. On a match, it sends the completion signal. On a mismatch, it stops the trial: the two ends disagree on `K`, `T`, or `code_seed`.

Check: one object crosses the real path, including every relay in its final place, and the checksum matches. Loss may require more batches than stage 1. The source continues until the completion signal.

## Stage 4. Recoding, one relay at a time

Replace forwarding with `bats_recode` at the first relay. Leave the others forwarding. Repeat the stage 3 transfer and require the checksum to match. Then convert the next relay. The destination never recodes.

At a recoding relay:

1. Keep a bucket keyed by flow, generation, and `batch_id`. One `bats_recode` call receives packets of one key only.
2. Keep one `uint64_t recode_state` for each flow on that hop. Set it to a nonzero value before the first recode of that flow, and pass the same variable on every later batch. The library updates it. Another hop, and another flow, each have their own variable.
3. Drop a packet whose coefficient is already in the bucket. Pass the remaining packets in arrival order: packet `s` occupies `coeff_in[s * 8]` and `payload_in[s * T]`.
4. Recode and forward as soon as the bucket holds eight packets. When the hop becomes idle and the bucket holds from one to seven packets, recode those packets. An empty bucket produces no output. A call with `n_in == 0` writes nothing and sets `*n_out` to 0. Otherwise the call writes eight packets and sets `*n_out` to 8.
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

Check, after each relay is converted: the same object arrives, and the checksum matches. A relay still forwards during this stage when it has not yet been converted.

## Stage 5. Accept the deployment

Run the object again with every relay recoding and with the built-in Ψ. Accept the deployment when the checksum matches. A return of 1 from `bats_decode` with a failed checksum means the source and the destination used different parameters. Correct those parameters and repeat this stage. Leave Ψ unchanged until this check passes.

After this check, a larger object is a sequence of generations. Each generation has its own `batch_id` sequence, starting at 0, and its own symbol array at the destination. A relay bucket includes the generation in its key. `recode_state` stays with the flow and is not reset at a generation boundary.

A second flow is the same path with a new flow identifier, a new `recode_state` on each hop, and its own `batch_id` sequence. The header already has the flow field from stage 2.

## Stage 6. Degree distribution for this network

Collect this measurement on the path that passed stage 5, with the same relays recoding, and with the Ψ that stage 5 used. The built-in distribution is the correct first measurement. Keep that Ψ for the whole measurement.

The rank of a batch is the rank over GF(256) of the coefficient rows that arrived for that `batch_id`. Place each received 8-byte coefficient in a row of an `r × 8` matrix and row-reduce it. The number of nonzero rows is the rank. It is a property of the coefficients, so extra packets can be linearly dependent and the packet count is a different quantity. A batch that the source finished sending, and from which nothing arrived, has rank 0. A batch the source has not sent is left out of the histogram.

The destination counts how many batches have each rank from 0 through 8. The result is nine integers, `h0` through `h8`. Continue until those nine integers cover several hundred batches. One short object may finish in fewer batches; repeat generations on the same path until the count is large enough. Both ends keep the same Ψ for every one of those generations.

Build the optimizer in the `bats` directory. The degree cap is at most `K`. A larger degree is reduced to `K` during encoding.

```bash
make psi_opt
./psi_opt --eta-bar 0.99 --max-degree 128 --weights-out psi_network.txt \
    h0 h1 h2 h3 h4 h5 h6 h7 h8
```

`--eta-bar` is the fraction of source packets that belief propagation is required to sustain. The default is 0.99. The optimizer writes `psi_network.txt` within a few seconds. The first value in the file is the maximum degree `D`. The following line contains the integer weights of degrees 1 through `D`. Lines beginning with `#` are comments.

Check: the file exists, `D` is at least 1, and the nine ranks sum to the number of finished batches.

## Stage 7. Install Ψ and transfer again

Copy `psi_network.txt` to the source host and the destination host. After `bats_init`, and before the first packet of the next transfer, both ends load the same weights. A relay does not load them. `psi_set` stores one distribution for the process and returns 1 on success.

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

Run the stage 5 transfer again. The checksum matches only when both ends installed this file. Updating one end alone makes the destination solve for the wrong packets, and `bats_decode` can still return 1.

Replace the file only between transfers, and replace it at both ends in the same step. When the path, the number of relays, or the loss changes, repeat stage 6 with the weights then in use, run `psi_opt` again, and install the new file at both ends together.
