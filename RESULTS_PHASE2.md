# Fase 2 — gli esperti eseguiti dai peer: risultati misurati

Data: 2026-08-04. Macchina: WSL2, 12 core logici / 6 fisici, 25 GB RAM.
Modello: `tiny_olmoe` (fixture sintetica, pesi casuali) — hidden 1024,
inter 1024, 16 layer, 8 esperti/layer, top-4, esperto = 3.1 MB int8.
Comando: `./phase2_test.sh`.

## Cosa è stato messo alla prova

Lo stesso modello, lo stesso prompt, generato due volte:

- **A (locale)** — il motore legge i pesi degli esperti e li esegue lui.
- **B (P2P)** — il motore tiene solo densi + router + KV; ognuno dei 128
  esperti vive su uno di **4 processi peer** e viene eseguito lì; sul filo
  viaggiano solo attivazioni (4 KB andata, 4 KB ritorno).

I due lati non re-implementano nulla: `expert_node.c` fa `#include` di
`olmoe.c` (stesso idioma dei test del progetto) e chiama **lo stesso**
`matmul_q` sugli stessi pesi caricati dallo stesso loader. Locale e remoto
non possono divergere perché sono un'unica sorgente.

## Risultati

| | locale | P2P (4 peer) |
|---|---|---|
| token generati | 24 | 24 |
| **token identici** | — | **24 / 24** |
| velocità | 7.33 tok/s | **7.29 tok/s** (−0.5%) |
| RSS di picco del chatter | 0.64 GB | **0.27 GB** |
| esperti in cache locale | 93.5% hit | **nessuno** |

**Il risultato che conta è la prima riga**: la rete non ha cambiato un solo
token. L'invariante di colibrì — il placement può cambiare la velocità, mai
la semantica — sopravvive al passaggio in rete.

**Il secondo risultato è la RAM**: il chatter passa da 0.64 a 0.27 GB perché
non tiene, non legge e non quantizza nessun esperto. Su un modello vero è la
differenza tra "gli esperti sono il 90% del checkpoint" e "non li ho mai".

## Scomposizione del costo (probe_rtt)

Una singola chiamata a un peer, misurata su 200 ripetizioni:

```
ping  0.272 ms   round trip TCP vuoto (protocollo)
exec  0.622 ms   stesso round trip con 1024 float + l'esperto eseguito
      → 0.35 ms sono l'esperto, 0.27 ms il trasporto
```

Round di layer completo (4 esperti in parallelo): **5.13 ms**. Non è la rete:
è la contesa di CPU, perché i 4 peer e il chatter si dividono 6 core fisici.
Su macchine separate questo numero collassa verso il singolo `exec`.

**Trappola trovata e chiusa**: un peer che lascia decidere a OpenMP il numero
di thread (default = tutti i core logici) crea una squadra fredda dentro ogni
thread di connessione — **7.47 ms per esperto invece di 0.62**, 12× peggio.
I peer vanno avviati con `OMP_NUM_THREADS` esplicito.

---

# Banco di prova onesto (2026-08-05) — `phase2_bench.sh`

Il test qui sopra provava la correttezza, ma il suo tok/s valeva poco: peer e
chatter si contendevano gli stessi core, e il baseline era "esperti già in
RAM", cioè lo scenario in cui lumabri non servirebbe. Due correzioni:

- **Core partizionati** con `taskset`: chatter sui core fisici 0-2, ogni peer
  su un core fisico suo (3, 4, 5). Nessun peer ruba cicli al chatter — che
  anzi ne ha **meno** di prima, per non falsare il confronto a proprio favore.
- **Il baseline giusto**: esperti streammati dal disco (`CACHE=1` +
  `EXPERT_DROP=1`, cioè `fadvise(DONTNEED)` dopo ogni lettura), che è ciò che
  fa davvero un modello da 167 GB.

Modello `olmoe_bench`: hidden 2048, inter 1024, 16 layer, 16 esperti, **top-8**
— esperto da **6.3 MB** e **128 chiamate remote per token**, gli stessi numeri
di OLMoE-1B-7B reale. Solo il bacino di esperti è più piccolo (16 invece di 64).

La rete è emulata **dentro il peer** (`LUMABRI_RTT_US` / `JITTER_US` /
`LOSS_PPM`): trattiene la risposta per il tempo di volo prima di spedirla.
Fedele per quel che si misura, perché le K richieste di un layer viaggiano già
su K socket con K thread, quindi le attese si sovrappongono e un round costa
un RTT e non K. Nessuna modifica al sistema operativo.

| | tok/s | RSS del chatter | round di layer |
|---|---|---|---|
| **A** locale, esperti residenti in RAM | 1.92 | 2.53 GB | — |
| **B** locale, esperti dal disco *(il baseline vero)* | **0.04** | 1.12 GB | — |
| **C** P2P, localhost puro | **7.02** | **1.04 GB** | 4.73 ms |
| **C** P2P, **LAN gigabit** (0.25 ms ± 0.05) | **5.97** | 1.04 GB | 5.39 ms |
| **C** P2P, **Internet** (30 ms ± 5, 0.1% perdita) | **1.13** | 1.04 GB | 40.06 ms |

**Token identici in tutti e cinque i percorsi.**

### Le tre cose che questi numeri dicono

**La LAN costa il 15%.** 5.97 contro 7.02 tok/s: su una rete di casa il P2P
funziona quasi come se i peer fossero nello stesso processo. È lo scenario per
cui l'architettura è disegnata, ed è quello che regge.

**Internet costa 6×, come previsto dal conto.** 1.13 tok/s con 16 layer in
sequenza a 30 ms. Il muro della latenza è reale e va aggredito col draft
speculativo (un round di layer per più token accettati), non con la rete.
Resta comunque **28× più veloce dello streaming da disco**.

**Lo straggler è misurabile, ed è peggio dell'RTT nominale.** Con 30 ms di
RTT il round di layer costa **40 ms**, non 30: si aspetta il più lento di 8
richieste, e il jitter di ±5 ms sul massimo di 8 campioni pesa +33%. È la
conferma sperimentale del rischio che il design segnalava: senza richieste
ridondanti (chiedi a 2 repliche, prendi la prima) la coda della distribuzione
detta la velocità.

### Nota sulla varianza del baseline B

Due esecuzioni dello stesso run B hanno dato 0.18 e 0.04 tok/s. È lo streaming
da disco a essere instabile — dipende da cosa la page cache ha ancora in mano
e da chi altro usa il VHDX — non la misura del P2P, che si è ripetuta entro il
2%. Il confronto va quindi letto come "uno o due ordini di grandezza", non
come un fattore preciso.

### Perché il P2P batte anche il caso "tutto in RAM"

Non è magia e va detto chiaramente: **i peer non portano solo memoria, portano
CPU**. Il run A usa 3 core fisici; il run C ne usa 3 (chatter) + 3 (peer), e
gli 8 esperti di un layer vengono calcolati davvero in parallelo mentre il
chatter aspetta. Non è un confronto a parità di hardware — è esattamente il
patto di lumabri: i maintainer donano macchine, e la rete guadagna il loro
calcolo oltre al loro disco.

Il numero che conta per la tesi del progetto resta l'altro: **33× più veloce
del disco**, con il chatter che scende a 1.04 GB perché tiene solo i densi.

### Costo per round di layer

496 round (31 posizioni × 16 layer), 3968 chiamate remote, 2.51 s di attesa
totale → **5.05 ms per round** con 8 esperti su 3 peer. Con `ping` a 0.27 ms,
il trasporto è ~5% del round: il resto è calcolo dell'esperto. È il motivo per
cui una LAN vera (0.3–0.5 ms di RTT) cambierebbe poco questi numeri, mentre
Internet a 30 ms li cambierebbe molto (16 × 30 ms = 480 ms per token ≈ 2 tok/s,
da recuperare col draft speculativo).

## Cosa questo NON dimostra (limiti onesti)

1. **Tutto su una macchina.** La contesa di CPU è stata eliminata coi core
   partizionati, ma il round trip resta localhost (0.27 ms): vicino a una LAN
   gigabit come valore medio, ma **senza jitter né perdita di pacchetti**.
   Per aggiungerli servono i privilegi di root, una riga sola:

   ```sh
   sudo tc qdisc add dev lo root netem delay 0.25ms 0.05ms   # LAN realistica
   sudo tc qdisc add dev lo root netem delay 30ms 5ms loss 0.1%   # Internet
   sudo tc qdisc del dev lo root                              # rimuovere
   ```

   `phase2_bench.sh` rileva il qdisc e lo stampa nell'intestazione, così un
   risultato non può essere attribuito alla rete sbagliata.
2. **Modello sintetico e piccolo.** L'esperto è 3.1 MB contro i 6.3 MB di OLMoE
   reale, e sono 8 esperti per layer invece di 64. La conta dei round di layer
   (16) è invece quella vera.
3. **Il baseline locale era il caso migliore.** 93.5% di hit: gli esperti erano
   di fatto residenti in RAM. Contro il caso realistico — esperti streammati da
   SSD — il P2P vincerebbe di molto, non pareggerebbe.
4. **Nessuna gestione degli straggler.** Un peer lento blocca il round del
   layer: servono richieste ridondanti e failover, che qui non ci sono.
5. **I float viaggiano grezzi**, assumendo la stessa architettura ai due capi.
6. **Nessuna verifica dei risultati.** Un peer malevolo può restituire numeri
   sbagliati e nessuno se ne accorge. È il problema aperto più serio della
   fase 2, e non ha una soluzione perfetta: solo ridondanza a campione più
   reputazione.

## Cosa dice, allora

Che l'architettura regge dove poteva rompersi: **spostare gli esperti su altre
macchine non cambia il modello, e non costa velocità misurabile** — su questa
macchina, in questo scenario. Il collo di bottiglia si sposta dove volevamo
che andasse: sulla parte densa, che è venti volte più piccola e sta in RAM.
