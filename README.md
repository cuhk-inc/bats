# BATS

BATS (BATched Sparse codes) over GF(256). This directory is a codec library plus an in-process simulator. Link the library into another program. The simulator is only for checking the code on a fake four-node path.

The library does three things: `bats_encode`, `bats_recode`, and `bats_decode`. It does not add precoding, open sockets, or define a packet header. The caller carries `code_seed`, `batch_id`, `K`, and `T` to the other side. How to use the codec, including relays and the degree distribution on a new network, is in `INTEGRATION.md`.

## Coding scheme

Split the source into `K` packets of `T` bytes each. A batch does not mix all `K` of them. It mixes a small subset, then emits `BATS_M` (8) coded packets. Many batches, each about a different subset, cover the object. That is the sparse part of the code: decoding can start from a batch whose subset is small enough to solve, then use those recovered packets to simplify the batches that overlap it.

Each call to `bats_encode` builds one batch:

1. Draw a degree `d` from Ψ. If `d` is larger than `K`, use `K`.
2. Choose `d` source packets uniformly, then fill a `d × 8` matrix `G` with uniform GF(256) entries.
3. The 8 payloads are those `d` packets times `G`. Packet `j` is sent with the `j`-th column of the `8 × 8` identity as its coefficient.

`G` and the chosen indices are not in the packet. For each `batch_id` the library opens an RNG stream that depends only on `code_seed` and that id. The first draw is `d`, the next draws pick the `d` packets, and the draws after that fill `G`. A lost batch does not shift the stream of any other batch. Both ends must run this same sequence. The received 8-byte coefficient says how the original 8 payloads were mixed by relays; the destination combines it with each row of `G` to get the equation for one selected source packet.

A relay does not draw Ψ. It mixes packets that already share one `batch_id`, including a batch that has not fully arrived. Packets from two batch ids must not go into one `bats_recode` call, because they were built from two different subsets.

### Degree distribution

Ψ gives the probability of each degree. `weights[d] / sum(weights)` is the probability of degree `d`. `weights[0]` is unused.

Degree is the number of source packets inside one batch. It is also the number of unknowns that batch starts with. Belief propagation solves a batch only when the rank of what arrived is equal to the number of source packets in it that are still unknown. Eight packets can supply rank at most 8, so a batch with degree above 8 cannot be solved from its own packets alone, even if all 8 arrive. It waits until other batches have recovered some of its packets and those known packets have been substituted out. A degree near 1 solves from very little data and unlocks overlapping batches, but each such batch covers little of the object, so more batches are required. Ψ is the balance between those two.

The built-in Ψ stays inside degrees 1 through 8, so a complete batch is immediately eligible for belief propagation. Its weights are 12, 30, 40, 44, 40, 36, 30, 24 and sum to 256. The largest weights sit on degrees 3, 4, and 5. The mean degree is 1186/256. Call nothing and both ends use this distribution.

Replace it only when both ends install the same weights. The degree is the first value taken from the batch RNG, so a different Ψ changes `d`, then changes which source packets are chosen and what `G` is. The destination then writes equations against the wrong packets. `bats_decode` does not compare its result with the original object. It returns `1` when the equations it built have a solution. Those equations describe the real source packets only when Ψ matches. A checksum, if you want one, belongs in the caller.

`psi_set` stores one distribution for the whole process. Set it before any encode or decode, on every process that does either. Recode does not read it.

### Decoding

The destination runs belief propagation across batches. Solving one batch substitutes those source packets into every other batch that contains them, which can drop another batch down to a solvable size. When no batch can move, inactivation picks one still-unknown packet, sets it aside, and lets peeling continue. The packets set aside are solved together in one dense system at the end. Decoding has succeeded only when every one of the `K` source packets is recovered, whether by peeling or by that final system.

## Using the library

```bash
make lib
```

That builds `libbats.a` from `field.c`, `source.c`, `dest.c`, and `codec.c`. Include only `bats_codec.h`:

```bash
gcc -std=c11 -I/path/to/bats caller.c /path/to/bats/libbats.a -o caller
```

Do not link `link.c`, `relay.c`, `sim.c`, `main.c`, or `psi_opt.c`. Call `bats_init` once before the first encode, recode, or decode.

`bats_encode` writes 8 packets. The source buffer is `K * T` contiguous bytes.

| Buffer | Size | Layout |
| --- | --- | --- |
| `coeff` | `8 * 8` | Packet `j` starts at `coeff[j * 8]` |
| `payload` | `8 * T` | Packet `j` starts at `payload[j * T]` |

`bats_encode` returns `0` on success and `-1` when `K < 1`, `T < 1`, or a pointer is `NULL`.

```c
#include "bats_codec.h"

#include <string.h>

enum { K = 128, T = 32 };

void example(const uint8_t src[K * T]) {
    uint8_t coeff[BATS_M * BATS_M];
    uint8_t payload[BATS_M * T];
    uint8_t dst[K * T];
    BatsSymbol symbols[BATS_M];
    uint64_t code_seed = 1;
    uint32_t batch_id = 0;
    int j;

    bats_init();
    bats_encode(src, K, T, code_seed, batch_id, coeff, payload);

    for (j = 0; j < BATS_M; j++) {
        symbols[j].batch_id = batch_id;
        memcpy(symbols[j].coeff, coeff + j * BATS_M, BATS_M);
        symbols[j].payload = payload + j * T;
    }
    /* 1 means dst holds the original K packets.
       One batch is usually not enough; send further batches with batch_id + 1. */
    bats_decode(symbols, BATS_M, K, T, code_seed, dst, NULL, NULL);
}
```

The next batch uses the same `code_seed` and `batch_id + 1`. The library does not drop packets. The caller decides which packets arrive.

## What each packet must carry

The library does not build a header. Whatever transport you use, every packet needs three fields, and both ends must already agree on `K`, `T`, `code_seed`, and Ψ:

| Field | Size | Where it comes from |
| --- | --- | --- |
| `batch_id` | 4 bytes | The value passed to `bats_encode` |
| coefficient | 8 bytes | `coeff[j * 8]` for packet `j` |
| payload | `T` bytes | `payload[j * T]` for packet `j` |

The 8-byte coefficient is required after a relay as well. At the source it is a column of the identity. `bats_recode` replaces it with a new coefficient. Forward that new coefficient with the new payload. Dropping it leaves the destination unable to form equations.

The destination rebuilds `G` from `code_seed` and `batch_id`, then takes the inner product of each row of `G` with the received coefficient. That scalar is the equation coefficient of one selected source packet.

The same `code_seed` and `batch_id` always rebuild the same batch. Sending that `batch_id` again repeats those 8 packets. A new batch, and new equations, needs a new `batch_id`.

One batch touches only `d` of the `K` source packets and contributes at most 8 packets. For a large `K`, keep encoding with `batch_id + 1` and call `bats_decode` on everything received so far. Stop when it returns `1`. If the path loses packets, send further batches. Use `dst` only when the return value is `1`.

If the object length is not a multiple of `T`, pad the last source packet with zeros before encoding and drop that padding after a successful decode.

Both ends either leave `psi_set` unset or install the same weights. How that distribution enters the batch is described above.

## Recode

`bats_recode` mixes `n_in` packets of one batch into 8 new packets. Coefficient and payload input are packed the same way as the encode output: packet `s` occupies `coeff_in[s * 8]` and `payload_in[s * T]`.

`n_in == 0` writes nothing, sets `*n_out` to 0, and returns 0. Otherwise it writes 8 packets and sets `*n_out` to 8. Input and output buffers may overlap. A bad argument returns `-1`.

`recode_state` is an RNG state owned by the caller. Set it to a seed before the first recode on that hop, then pass the same variable for later batches on that hop. The library updates it. Give each hop its own state.

## Decode

`bats_decode` takes a flat `BatsSymbol` list. It groups symbols by `batch_id`, rebuilds `G` from `code_seed` and that id, then decodes.

| Return | Meaning |
| --- | --- |
| `1` | All `K` source packets are in `dst` |
| `0` | Not enough equations, or `n_symbols == 0` |
| `-1` | `K < 1`, `T < 1`, a required pointer is `NULL`, or a symbol has a `NULL` payload |

`n_bp` and `n_inact` may be `NULL`. When they are not, they receive how many source packets belief propagation solved and how many inactivation solved. Each `payload` pointer must stay valid until `bats_decode` returns; the library copies those bytes during the call. Symbol order does not matter. A repeated packet is just another row.

## Degree distribution

```c
int weights[] = {0, 12, 30, 40, 44, 40, 36, 30, 24};
psi_set(weights, 8); /* returns 1 on success */
```

`weights[0]` is unused. `weights[1 .. max_degree]` are non-negative integer weights, and `max_degree` is from 1 to 4096. The weights must not all be zero. `psi_set` returns `1` on success and `0` when the weights are rejected. Skipping the call keeps the built-in distribution.

## Simulator

`bats_sim` runs source, relay 1, relay 2, and destination inside one process. Loss is applied before a packet is queued. The default loss rate is 5 percent.

```bash
make
./bats_sim input.txt recovered.txt
./bats_sim --optimize input.txt recovered.txt
./bats_sim --psi psi.txt input.txt recovered.txt
```

`--optimize` and `--psi` cannot be combined. The program exits 0 when the recovered file matches the input, and 1 otherwise. Before the file run it self-checks GF(256) and inactivation, then searches for the smallest number of batches that recovers a `K = 128`, `T = 32` object on the same 5 percent path and prints the rank histogram at each hop.

`psi_opt` is the same degree-distribution optimizer, built on its own. On Windows, `build.bat` compiles both programs with `tools/tcc/tcc.exe` when that compiler is present.

## Files

| File | Role |
| --- | --- |
| `bats_codec.h` | Public API |
| `codec.c` | `bats_encode`, `bats_recode`, `bats_decode` |
| `field.c` | GF(256) arithmetic and the batch RNG |
| `source.c` | Degree distribution and `G` |
| `dest.c` | Belief propagation and inactivation |
| `link.c`, `relay.c`, `sim.c`, `main.c` | Simulator only |
| `psi_opt.c` | Degree-distribution optimizer |
