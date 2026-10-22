# Using BATS

Send an object with three calls. The source calls `bats_encode`. Each relay calls `bats_recode`. The destination calls `bats_decode`. Sockets, headers, and rate control stay in the caller.

One generation is `K` source packets of `T` bytes. A batch is eight packets, mixed from `d` of those source packets. `d` comes from the degree distribution Ψ. The mix is a matrix `G`. `G` and the chosen packet numbers are not written into the packet. The destination rebuilds them from `code_seed` and `batch_id`. The arithmetic is over GF(256).

## Source

Lay the generation out as `K * T` bytes. Pad the last packet with zeros if the object is shorter. Start at `batch_id` 0.

Each call sends eight packets and moves to `batch_id + 1`. Packet `j` carries column `j` of the 8 by 8 identity as its coefficient, and the mixed payload beside it. The coefficient buffer is 64 bytes, packet `j` at `coeff[j * 8]`. The payload buffer is `8 * T` bytes, packet `j` at `payload[j * T]`. The call returns 0 on success.

```c
bats_encode(src, K, T, code_seed, batch_id, coeff, payload);
```

Keep sending batches until the destination says the generation is done. A lost packet is replaced by a later batch, not by repeating an old `batch_id`. The next generation starts again at `batch_id` 0.

## Relay

A relay does not know which source packets are inside a batch, and it does not read Ψ.

Keep one bucket per flow, generation, and `batch_id`. One `bats_recode` call reads one bucket. Keep one `uint64_t recode_state` per flow on that hop. Set it to a nonzero value once, then pass the same variable on every later batch. The next hop has its own variable.

Drop a packet whose coefficient is already in the bucket. When the bucket holds eight packets, recode and forward. When the hop has been idle and the bucket holds from one to seven, recode those. On the line program the idle time is `--idle-sec`, default 15 seconds. An empty bucket forwards nothing.

The same weights multiply the coefficient and the payload. With at least one input packet the call writes eight new packets and sets `n_out` to 8. Forward each new coefficient with its new payload.

```c
bats_recode(coeff_in, payload_in, n_in, T, &recode_state,
            coeff_out, payload_out, &n_out);
```

Packet `s` occupies `coeff_in[s * 8]` and `payload_in[s * T]`. The input and output buffers may overlap.

## Destination

The destination does not recode. For each arrival, save `batch_id`, the eight coefficient bytes, and `T` payload bytes. Order does not matter. Each payload pointer stays valid until `bats_decode` returns.

```c
bats_decode(symbols, n_symbols, K, T, code_seed, dst, NULL, NULL);
```

A return of 0 means more packets are needed. A return of 1 means every source packet was solved. Strip the padding and compare a checksum of the original object. The solved bytes are the object only when both ends used the same `K`, `T`, `code_seed`, and Ψ. On a match, tell the source to stop. The library has no message of its own for that.

A return of `-1` means an argument is invalid. `n_bp` and `n_inact` may be `NULL`.

## Agree this before sending

The source and the destination use the same `K`, `T`, and `code_seed`. On the line program the defaults are `K = 128`, `T = 4096`, and `code_seed = 0xB175`. A relay ignores all three.

Every packet carries these fields. Multi-byte integers are big-endian.

| Field | Size |
| --- | --- |
| Flow | 2 bytes. One flow uses 0 |
| Generation | 4 bytes. The first generation is 0 |
| `batch_id` | 4 bytes |
| Coefficient | 8 bytes |
| Payload | `T` bytes |

The line program already packs them into its 44-byte header. Another program may keep its own header. These five values have to survive every hop. A second flow needs its own flow number, its own `batch_id` sequence, and its own `recode_state` on each hop.

Call `bats_init` once in each process before the first codec call.

## A degree distribution for a new network

Use one Ψ for a whole transfer. Relays never load it. Change it only between transfers, and change it at both ends together.

On the line program this takes two runs.

1. Run with no `psi_network.txt` in the working directory. The source and the destination use `psi_line.h`. Add `--fit-psi` on the destination only. Use `--loss-percent 0` so the measurement sees the real path. This run keeps the old distribution. At the end the destination writes `psi_network.txt`.
2. Copy that file into the source working directory. Run again without `--fit-psi`. Both ends load it and print `装入度数 psi_network.txt`.

```bash
./build/bats_line --role dest --listen 9000 --output /tmp/bats_out.bin \
    --k 128 --t 4096 --seed 0xB175 --idle-sec 15 --fit-psi
```

Use the same `--k`, `--t`, and `--seed` on the source. The checksum of the second object confirms that both ends loaded the file. Delete the file on both machines to go back to `psi_line.h`. When the path, the relay count, or the loss changes, run step 1 again and copy the new file over the old one.

`--fit-psi` is only for the destination. It cannot be combined with `--copy`. A file name other than `psi_network.txt` is passed back with `--psi` on both the source and the destination.

## Linking the codec into another program

```bash
make lib
gcc -std=c11 -I/path/to/bats caller.c /path/to/bats/libbats.a -o caller
```

The archive is `field.c`, `source.c`, `dest.c`, and `codec.c`. Include only `bats_codec.h`. Leave `psi_opt` as a separate tool. The library does not watch the network, so the program loads Ψ itself.

On the first transfer, call no `psi_set`. Both ends use the built-in weights 12, 30, 40, 44, 40, 36, 30, 24 for degrees 1 through 8. The destination counts how many batches have each rank from 0 through 8. Rank is the GF(256) rank of the coefficient rows that arrived for one batch. A finished batch with nothing received has rank 0. A batch the source has not sent is left out. Collect several hundred batches, then:

```bash
make psi_opt
./psi_opt --eta-bar 0.98 --max-degree 128 --weights-out psi_network.txt \
    h0 h1 h2 h3 h4 h5 h6 h7 h8
```

Use `--max-degree K` when `K` is below 128. The first number in the file is the maximum degree `D`. The next line is the weight of each degree from 1 through `D`. Lines that start with `#` are comments.

On the next transfer, after `bats_init` and before the first packet, the source and the destination both call `psi_set` with those weights. `weights[0]` is unused. The call returns 1 when the weights are accepted. A relay skips the call.
