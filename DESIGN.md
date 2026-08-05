# lumabri — design

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

## Protocollo (lumabri_proto.h)

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
- **Blocchi**: default 8 MiB (`LUMABRI_BLOCK_MIB`). Fetch con dedup
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

## Fase 3 — la guerra all'RTT (2026-08-05)

Il muro misurato in fase 2 (30 ms × layer sequenziali) non si abbatte con una
leva sola: si abbatte con leve moltiplicative. Tre sono implementate e
provate da `phase3_test.sh`; due restano sul tavolo.

**Topologia: perché non (ancora) un grafo/DHT.** La mappa vicino/lontano non
richiede una DHT: ogni nodo misura da sé i propri archi — due PING per peer
alla partenza (il secondo viaggia sulla connessione calda; si tiene il min) —
e il tracker resta un indice Napster che non sa dov'è nessuno. La prossimità
vive nel nodo che ne beneficia, l'unico posto dove si può misurare
onestamente. Una DHT diventa necessaria solo per la rete aperta a migliaia di
peer (SPOF del tracker, relay decentralizzato): il protocollo lo permette
senza stravolgimenti — PLACEMENT diventa una lookup — ma è lavoro di quella
fase, non di questa.

Le leve, con le misure (emulazione `LUMABRI_RTT_US` dentro il peer, stessa
metodologia del banco di fase 2):

1. **Replica più vicina, fase 1** (lumashim): i peer di un file ordinati per
   RTT misurato; chi sta entro il 25% + 2 ms del migliore è "ugualmente
   vicino" e i blocchi si spartiscono per hash tra loro; i lontani sono
   failover, il relay ultimo. Provato: replica piena a 0 ms + replica piena a
   60 ms → 25.2 MB dal vicino, **0 byte dal lontano**, byte-identico.
2. **Prefetch, fase 1** (`LUMABRI_PREFETCH`, default 2 blocchi, 0 spegne):
   la mossa di Spotify — il carico di un modello è quasi tutto sequenziale,
   quindi mentre il motore mastica il blocco N lo sciame sta già spedendo
   N+1..N+K su worker paralleli. Provato: mirror freddo su sciame a 40 ms,
   **45% più veloce** (1246 → 679 ms). La stessa `ensure_block` del path di
   lettura: la bitmap e l'in-flight dedup rendono impossibile il doppio
   fetch, e un prefetch fallito è silenzioso perché il read path lo ritenta
   rumorosamente se il blocco serve davvero.
3. **Repliche + vicinanza + failover, fase 2** (lumabri_client): fino a 4
   repliche per esperto, ogni chiamata alla più vicina viva; un peer che
   fallisce è marcato morto e l'esperto ritenta sulla replica successiva —
   mai fallback locale (l'invariante regge), fatale solo l'esperto senza
   repliche vive. Provato due volte: (a) esperto replicato a 2 ms e a 30 ms
   col lontano primo in lista → **10.48 tok/s contro 1.37** del solo-30ms
   (4.0 ms per round invece di 34.6); (b) peer ucciso a metà generazione →
   4 failover, **token identici** al riferimento locale.

La lettura: il muro dell'RTT è il muro della *replica più vicina*, non della
media dello sciame. Con sciami che clusterizzano geograficamente (città,
campus), il conto 30 ms diventa un conto 2-8 ms — e il resto lo devono fare
le due leve rimanenti:

4. **Draft speculativo con batch-union** (appunti in fase 2): un round di
   layer per l'intero draft → ×3-5. Richiede il drafting del motore: lavoro
   lato colibri, il protocollo qui è pronto (EXEC porta già più righe in
   linea di principio).
5. **Predizione degli esperti + cache LRU dei caldi sul chatter**: spedire le
   chiamate del layer N+1 in anticipo sulla predizione del router, e tenere
   localmente (via il mirror di fase 1) gli esperti più chiamati. Ogni hit è
   un round trip in meno.

## Fase 4 — bootstrap-and-delegate (2026-08-05)

Il problema del giorno zero: uno sciame appena nato non ha donatori, e senza
esecutori la fase 2 non parte. La politica: **il server esegue per primo,
delega man mano che i donatori arrivano, e resta l'ultima istanza.**

- **Il server è anche esecutore**: `lumabri serve` lancia un `expert_node`
  sull'intero modello. Gli esperti restano sull'SSD e passano da una LRU in
  RAM (`--cache N` slot): il metodo colibri applicato al peer. Acquire/pin
  con refcount (mai evizione sotto una matmul in corso), loader del motore
  serializzato da un mutex, un miss = una lettura NVMe. Identità provata con
  cache 8 su 128 esperti — 80% di cold load, zero byte cambiati.
- **Scoperta dal tracker** (EREG/EPEERS): gli expert node fanno heartbeat
  come i maintainer; il client chiede EPEERS{model} quando LUMABRI_EXPERTS
  non è impostata. Zero configurazione lato chatter.
- **La scala di degrado**, ogni gradino rumoroso, nessuno può cambiare un
  byte: replica più vicina → altra replica → ri-query del tracker (un
  donatore può essere arrivato *dopo* l'avvio) → e se all'avvio lo sciame
  non copre tutti gli esperti, la fase 2 resta spenta e il motore li esegue
  in locale dai byte del mirror di fase 1.

Misure di `phase4_test.sh`: identità con cache affamata (19.6% hit), 
bootstrap zero-config via tracker, donatore a metà modello ucciso in piena
generazione → failover al server, token identici al riferimento locale.
`make install` porta tutto in PREFIX/bin + PREFIX/lib/lumabri; i binari si
trovano l'un l'altro via /proc/self/exe.

Aperto, in ordine: hash per blocco (fase 1) e verifica a campione dei
risultati (fase 2) prima dei peer sconosciuti; batch-union speculativo lato
motore; assegnazione degli esperti dal tracker (oggi il donatore sceglie
con --stride/--layers; dovrebbe decidere il server, rarest-first come per i
byte); relay EXEC per esecutori dietro NAT.

## Fase 5 — sciame aperto e sciame a inviti (2026-08-05)

Due modelli di fiducia, entrambi completi.

**Aperto (chiunque entra, nessuno è fidato).** Catena di custodia radicata
nell'operatore dello sciame, mai nel peer che serve i byte:

- sha256 per MiB (`LMB_HASH_CHUNK`) calcolato dal maintainer, in cache in
  `.lumabri_hashes/<rel>.sha` (invalidata dal cambio di size): solo il primo
  avvio paga. Spedito dentro REGISTER come sezione opzionale (magic "SHAH",
  i peer vecchi semplicemente non la mandano).
- Il tracker tiene la **prima** dichiarazione di ogni (model, path) come
  verità — l'origine registra prima che esista un donatore — e **rimuove
  dall'offerta** di ogni registrante successivo i file i cui hash
  contraddicono: il veleno muore all'indice, non entra mai in un placement.
- Chatter e donatori-in-pull chiedono la verità al **tracker** (op HASHES) e
  verificano ogni blocco: il peer che mente si vede rifiutare i byte e il
  blocco viene ripreso altrove, rumorosamente. `LUMABRI_REQUIRE_HASH=1`
  rifiuta di scaricare dove la verità non c'è (modo severo per sconosciuti).
- Fase 2, `LUMABRI_VERIFY=N`: l'N% delle chiamate viene rieseguito su
  un'**altra replica** e deve tornare byte-identico. È qui che l'invariante
  di identità diventa un'arma: due peer onesti non possono discordare,
  quindi una discordanza *è* la prova di una menzogna — e la run si ferma
  invece di emettere un token di cui nessuno può rispondere.

**A inviti.** `LUMABRI_TOKEN` su ogni macchina: tracker, maintainer e
expert node rifiutano le connessioni non autenticate. Il token protegge
byte e calcolo, non solo l'indice.

Provato da `phase5_test.sh` con peer che mentono come mentirebbe un
avversario (manifest onesto, byte corrotti — `LUMABRI_CORRUPT_PPM`): 7
blocchi corrotti rifiutati con mirror byte-identico, poisoner spogliato
alla registrazione, esecutore bugiardo beccato al primo spot-check.

## Fase 6 — la firma: il tracker diventa corriere (2026-08-05)

Il limite della fase 5 era che **il tracker era l'autorità**: decide lui
quali byte sono veri, quindi comprometterlo significa riscrivere il
modello. La firma sposta l'autorità su una chiave che l'operatore tiene
offline (le firme si calcolano una volta sola).

- `lumabri key` genera una coppia ed25519: `.key` (segreto, 0600) e `.pub`.
- L'**origine** (`maintainer --key`, o `serve --key`) firma per ogni file il
  messaggio canonico `"lumabri-truth-v1\0" model \0 path \0 chunk size
  hashes` — il binding è il punto: senza model/path/size dentro la firma,
  una firma valida si potrebbe rigiocare su un altro file dello stesso
  sciame, che è l'errore classico. Il tag di dominio impedisce che una
  firma lumabri valga come firma di altro.
- Il **tracker** memorizza e inoltra la firma; con `--pubkey` (che
  `serve --key` gli passa da sé, derivandolo dal segreto) rifiuta ogni
  dichiarazione non firmata. Ma la sua verifica è difesa in profondità, non
  la garanzia.
- La garanzia è il **chatter**: ricostruisce da sé il messaggio firmato e lo
  verifica con la chiave ottenuta fuori banda (`LUMABRI_PUBKEY`). Un tracker
  compromesso può negare la verità, non riscriverla. Avere una chiave
  implica modo severo: i byte non firmati si rifiutano, non si annotano.

Cripto self-contained (`lumabri_sign.h`): SHA-512 e Ed25519 in stile
TweetNaCl, aritmetica a limbi da 16 bit su 2^255-19, swap condizionali
constant-time. Verificato da `sign_test.sh` contro il vettore RFC 8032,
contro `sha512sum`, e contro OpenSSL **in entrambe le direzioni** (le
nostre firme verificano lì, le sue verificano qui). Più lo sciame firmato
end-to-end: un peer non firmato non entra nell'indice, e un tracker che
inventa la verità viene beccato dalla chiave del chatter.

Restano scoperti: rotazione e revoca delle chiavi, e soprattutto
l'**esecuzione** degli esperti, che oggi è garantita dall'accordo tra
repliche (fase 5) e non da una firma — un peer non può firmare un calcolo
che dipende dall'input, servirebbe attestazione o prova, ed è un problema
aperto in tutta la letteratura.

## Fase 2 (esperti remoti) — appunti

Il punto d'aggancio nel motore è il path `expert_load`/`ColiExpertStore`:
già oggi è una sorgente di byte intercambiabile (disco, mirror dual-SSD).
Un backend "peer" che spedisce l'attivazione invece di chiedere i byte
riusa: l'indice del tracker, il pool/failover dello shim, il batch-union
(un round per layer per l'unione degli esperti del draft) e la LRU separata
del drafter (DSpark) perché il drafting non deve pagare rete. La verifica
resta quella di colibrì: il draft si verifica sempre, la rete non tocca la
semantica.
