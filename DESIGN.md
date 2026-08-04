# lumibri — design

Data: 2026-08-04. Fase 1 (distribuzione P2P dei byte) funzionante in locale.

## Il problema e la scelta

Obiettivo: chiunque deve poter chattare con un MoE gigante senza scaricare
centinaia di GB prima, e chiunque deve poter "fare il maintainer" donando
disco/banda, come si condivideva musica con Napster.

Due varianti studiate:

- **A — i peer distribuiscono i byte** (questa fase). La rete è un tier
  *sotto* l'SSD del chatter: banda-limitata da freddo (100 Mbps–1 Gbps casa),
  velocità locale piena da caldo. Nessun problema di latenza per token,
  nessun leak di conversazione, integrità verificabile con hash.
- **B — i peer eseguono gli esperti** (fase 2). Attivazioni da ~4-8 KB per
  esperto: banda irrilevante, ma i layer sono sequenziali → il RTT domina
  (43 layer × 30 ms ≈ 1.3 s/forward). Si amortizza col draft speculativo
  (batch-union: un round trip per layer per l'intero draft da 5 token →
  ~2 tok/s stimati su RTT 30 ms). Problemi aperti: straggler (il layer
  aspetta il più lento dei k peer), churn, verifica di output non fidati,
  privacy delle attivazioni.

Scelta MVP: A, perché ogni pezzo (indice, protocollo, failover, cache) serve
identico anche a B, e A è utile da sola (il problema "862 GB di download").

## Perché LD_PRELOAD e non FUSE

FUSE paga kernel→demone→kernel su OGNI lettura, per sempre, anche a cache
calda, e richiede libfuse + privilegi. Lo shim paga solo al primo tocco:

- `open(vroot/x)` → apre il **mirror sparse** `cache/data/x` (creato con
  `ftruncate` alla dimensione vera) e registra l'fd in una tabella.
- `pread(fd, …)` su fd registrato → garantisce che i blocchi coperti dal
  range siano presenti (bitmap), poi delega alla `pread` vera.
- `fopen` su file del modello (config/tokenizer, piccoli) → fetch completo,
  poi `fopen` reale del mirror: le `fread` successive sono native.
- `opendir(vroot)` → `opendir(cache/data)`: `readdir` è nativa.
- `fstat`, `posix_fadvise`, `lseek`, page cache: nativi (l'fd è vero).

Superficie misurata sul binario deepseek (`objdump -T`): `open, fopen,
pread, fstat, close, opendir, readdir, posix_fadvise` — gli hook coprono
open/open64/openat/openat64, fopen/fopen64, opendir, pread/pread64, read,
close. `read()` è coperto per robustezza (tool esterni); i motori usano
`pread`.

Costo a caldo: un lookup `fdmap[fd]` + un test di bitmap per blocco toccato.

## Protocollo (lumibri_proto.h)

Frame: `{u32 magic "LMB1", u32 op, u32 body_len, u32 pay_len}` + body + pay,
little-endian, stringhe con prefisso u16. Cap: body 16 MiB, pay 64 MiB —
ogni allocazione è limitata prima di leggere. Op sconosciuta → `ERR`
esplicito (mai indovinare).

| op | flusso | uso |
|---|---|---|
| `PING` → `OK` | * | liveness |
| `MANIFEST` → `MANIFEST_R` | chatter→maintainer | file custoditi {rel, size} |
| `READ{path,off,len}` → `READ_R(pay)` \| `ERR` | chatter→maintainer | un range |
| `REGISTER{name,addr,files}` → `OK` | maintainer→tracker | heartbeat 10 s |
| `PLACEMENT` → `PLACEMENT_R` | chatter→tracker | file → {size, peer…} |

Il tracker è **solo indice** (Napster docet): mai un byte di modello, stato
in RAM, restart gratis (tutti si ri-registrano entro un heartbeat). Peer
silenzioso da >30 s → escluso dai placement.

## Lato chatter (lumishim.c)

- **Mirror**: `cache/data/<rel>` sparse + `cache/maps/<rel>.lmap` (1 byte
  per blocco, persistita con `pwrite` dopo il blocco: prima i dati, poi il
  bit — un crash rifetcha, mai zeri spacciati per dati).
- **Blocchi**: default 8 MiB (`LUMIBRI_BLOCK_MIB`). Fetch con dedup
  in-flight (mutex+condvar per file): N thread del motore che toccano lo
  stesso blocco = un solo fetch.
- **Peer**: pool di 4 connessioni persistenti per peer; scelta per blocco
  via FNV(rel,blk) % npeers (spreading deterministico); failover sugli
  altri peer del file; socket morto → 1 retry fresco.
- **Manifest persistito** (`cache/maps/manifest.txt`): a mirror caldo si
  lavora **offline** — placement non raggiungibile → si serve dalla cache,
  un miss freddo è `EIO` rumoroso.
- **Stato chatter-locale**: file NON nel manifest sotto la vroot (es.
  `.coli_usage` che i motori scrivono nella dir del modello) vanno nel
  mirror, scrivibili: la cronologia di routing appartiene al chatter.

## Invarianti

1. La rete cambia solo *da dove* arrivano i byte, mai *quali*: EROFS sulle
   scritture ai file del modello, EIO sui miss non servibili, mai zeri.
2. Il motore non è modificato e non è a conoscenza della rete.
3. A mirror caldo, il costo per lettura ≈ costo locale (page cache inclusa).
4. Selftest: cold = warm = offline, byte-identici (test_shim).

## Limiti noti (MVP)

- Nessuna verifica hash dei blocchi → solo peer fidati/localhost per ora.
  Primo requisito per la rete aperta: manifest firmato con sha256 per blocco.
- Niente NAT traversal (serve rendezvous UDP/QUIC per peer domestici).
- `dup()`/`fork()+exec` sull'fd del modello non tracciati (i motori non li
  usano sugli shard; LD_PRELOAD sopravvive comunque all'exec).
- fd ≥ 65536 su file del modello → EMFILE (limite tabella).
- Un solo vroot per processo.

## Fase 2 (esperti remoti) — appunti

Il punto d'aggancio nel motore è il path `expert_load`/`ColiExpertStore`:
già oggi è una sorgente di byte intercambiabile (disco, mirror dual-SSD).
Un backend "peer" che spedisce l'attivazione invece di chiedere i byte
riusa: l'indice del tracker, il pool/failover dello shim, il batch-union
(un round per layer per l'unione degli esperti del draft) e la LRU separata
del drafter (DSpark) perché il drafting non deve pagare rete. La verifica
resta quella di colibrì: il draft si verifica sempre, la rete non tocca la
semantica.
