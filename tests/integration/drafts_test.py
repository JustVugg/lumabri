#!/usr/bin/env python3
"""Drafts across the segments: identical replies in fewer network rounds.

Real tracker, two real segment processes per family and the real chatter on
loopback. For every family the reply with drafts must equal the reply without
them token for token (greedy and seeded sampling), the chain must take fewer
than one network round per decoded token, and a chain with one segment that
does not offer provisional rows must fall back to plain decode. Fixtures are
tiny random checkpoints: this proves the mechanism, not a speed.
"""
import argparse
import contextlib
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def wait_port(port, process, log):
    for _ in range(400):
        if process.poll() is not None:
            raise RuntimeError(f'process exited early:\n{Path(log).read_text()[-4000:]}')
        with socket.socket() as s:
            s.settimeout(0.2)
            if s.connect_ex(('127.0.0.1', port)) == 0:
                return
        time.sleep(0.025)
    raise RuntimeError(f'port {port} never opened:\n{Path(log).read_text()[-4000:]}')


class Household:
    """One tracker and one segment process per layer range."""

    def __init__(self, tmp, family, model_dir, ranges, env, plain=()):
        self.tmp, self.family, self.model_dir, self.env = Path(tmp), family, model_dir, env
        self.ranges, self.plain, self.processes = ranges, set(plain), []
        self.model = 'drafts-' + family
        self.model_root, self.tokenizer_root = '%064x' % 0xd1, '%064x' % 0xd2

    def spawn(self, name, argv, env):
        log = self.tmp / (name + '.log')
        process = subprocess.Popen(list(map(str, argv)), env=env, stdout=open(log, 'wb'),
                                   stderr=subprocess.STDOUT)
        self.processes.append((process, log))
        return process, log

    def __enter__(self):
        keys = {'LUMABRI_PEER_KEY': str(self.tmp / 'peer.key'),
                'LUMABRI_KNOWN_HOSTS': str(self.tmp / 'known_hosts')}
        self.env = {**self.env, **keys}
        self.tracker = free_port()
        process, log = self.spawn('tracker', [ROOT / 'tracker', '--port', self.tracker,
                                              '--peer-bindings', self.tmp / 'bindings'],
                                  {**self.env, 'LUMABRI_PEER_KEY': str(self.tmp / 'tracker.key')})
        wait_port(self.tracker, process, log)
        for index, (begin, end) in enumerate(self.ranges):
            port = free_port()
            env = dict(self.env)
            # Exactly how a segment that predates drafts answers.
            if index in self.plain:
                env['LUMABRI_DRAFT_ROWS'] = '0'
            process, log = self.spawn(f'segment-{index}', [
                ROOT / 'segment_node', '--engine', self.family, '--model-dir', self.model_dir,
                '--model', self.model, '--range', f'{begin}:{end}', '--port', port,
                '--tracker', f'127.0.0.1:{self.tracker}', '--advertise', f'127.0.0.1:{port}',
                '--name', f'{self.family}-{index}', '--model-root', self.model_root,
                '--tokenizer-root', self.tokenizer_root, '--context', '128',
                '--max-rows', '16', '--sessions', '4', '--fallback'], env)
            wait_port(port, process, log)
        return self

    def __exit__(self, *unused):
        for process, _ in self.processes:
            process.kill()
        for process, _ in self.processes:
            process.wait()

    def logs(self):
        return '\n'.join(f'--- {log.name}\n{log.read_text(errors="replace")[-3000:]}'
                         for _, log in self.processes)

    def argv(self, *extra):
        return [str(ROOT / 'segment_chat'), '--engine', self.family, '--model-dir',
                str(self.model_dir), '--model', self.model, '--tracker',
                f'127.0.0.1:{self.tracker}', '--model-root', self.model_root,
                '--tokenizer-root', self.tokenizer_root, '--context', '128',
                '--max-rows', '16', '--direct-only', *map(str, extra)]

    def chat(self, prompt, tokens, drafts, *extra, script=None):
        argv = self.argv('--prompt-ids', ','.join(map(str, prompt)), '--tokens', tokens,
                         '--json', '--drafts', drafts, *extra)
        env = dict(self.env)
        if script:
            env['LUMABRI_DRAFT_SCRIPT'] = ','.join(map(str, script))
        done = subprocess.run(argv, env=env, capture_output=True, text=True, timeout=180)
        if done.returncode:
            raise RuntimeError(f'{argv}: exit {done.returncode}\n{done.stderr[-4000:]}\n{self.logs()}')
        return json.loads(done.stdout.strip().splitlines()[-1])

    def chat_text(self, text, tokens):
        done = subprocess.run(self.argv('--prompt', text, '--tokens', tokens, '--json', '--drafts', 'off'),
                              env=self.env, capture_output=True, text=True, timeout=180)
        if done.returncode:
            raise RuntimeError(f'chat exit {done.returncode}\n{done.stderr[-4000:]}\n{self.logs()}')
        return json.loads(done.stdout.strip().splitlines()[-1])['token_ids']

    def serve(self, drafts, turns, script=None):
        """One resident conversation through the hosted codec.

        Each turn is (prompt, tokens); a callable prompt receives the replies
        so far, so a turn can extend exactly what the conversation holds.
        Returns the reply bytes, the DONE lines and the chatter's log."""
        env = dict(self.env)
        if script:
            env['LUMABRI_DRAFT_SCRIPT'] = ','.join(map(str, script))
        log = self.tmp / f'serve-{drafts}-{len(list(self.tmp.iterdir()))}.log'
        with open(log, 'wb') as stderr:
            process = subprocess.Popen(self.argv('--serve', '--drafts', drafts), env=env,
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr)
            replies, stats = [], []
            try:
                for index, (prompt, tokens) in enumerate(turns):
                    request = index + 1
                    text = (prompt(replies) if callable(prompt) else prompt).encode('latin-1')
                    process.stdin.write(b'SUBMIT %d 0 %d %d 0 0.95\n' % (request, len(text), tokens) + text + b'\n')
                    process.stdin.flush()
                    reply = b''
                    while True:
                        line = process.stdout.readline()
                        if not line or line.startswith(b'ERROR'):
                            raise RuntimeError(f'serve turn {request}: {line!r}\n'
                                               f'{log.read_text(errors="replace")[-4000:]}\n{self.logs()}')
                        if line.startswith(b'DATA %d ' % request):
                            size = int(line.split()[2])
                            reply += process.stdout.read(size)
                            process.stdout.read(1)
                        elif line.startswith(b'DONE %d STAT ' % request):
                            stats.append(line.decode('ascii').strip())
                            break
                    replies.append(reply)
            finally:
                process.stdin.close()
                process.wait(timeout=60)
        return replies, stats, log.read_text(errors='replace')


TEXT = ''.join(chr(33 + (i * 7) % 20) for i in range(12)) * 2


def drafts_stat(stat):
    """DRAFTS1 rounds tokens proposed accepted discards, or None."""
    fields = stat.split()
    if 'DRAFTS1' not in fields:
        return None
    at = fields.index('DRAFTS1')
    names = ('rounds', 'tokens', 'proposed', 'accepted', 'discards')
    return dict(zip(names, map(int, fields[at + 1:at + 6])))


def spoil(reply, every):
    """The reply as a draft script, with every `every`-th proposal made wrong."""
    return [token if (index + 1) % every else (4 if token == 3 else 3)
            for index, token in enumerate(reply)]


def check_metrics(family, metrics, tokens):
    assert metrics['tokens'] == tokens - 1, metrics
    assert metrics['accepted'] == metrics['tokens'] - (metrics['rounds'] - metrics['discards']), metrics
    assert metrics['accepted'] <= metrics['proposed'], metrics
    # A rejected draft is a discarded row at every segment; a gate in which
    # none was rejected would not have exercised the rollback.
    assert metrics['accepted'] < metrics['proposed'], f'{family}: no draft was ever rejected {metrics}'


def check_family(tmp, family, model_dir, ranges, env, prompt, tokens, lookup_pays, samples):
    with Household(tmp / 'full', family, model_dir, ranges, env) as home:
        plain = home.chat(prompt, tokens, 'off')
        assert 'draft_metrics' not in plain, plain
        assert len(plain['token_ids']) == tokens, plain

        def same(reply, what):
            assert reply['token_ids'] == plain['token_ids'], \
                f'{family}: {what} changed the reply\n{plain["token_ids"]}\n{reply["token_ids"]}\n{home.logs()}'
            return reply['draft_metrics']

        # Prompt lookup: needs no weights, so it is there for every family.
        lookup = same(home.chat(prompt, tokens, 'lookup'), 'prompt lookup')
        check_metrics(family, lookup, tokens)
        if lookup_pays:
            assert lookup['accepted'] and lookup['rounds_per_token'] < 1, lookup
        # Scripted drafts, mostly right: what a draft head's proposals look
        # like to the chain. One in six is wrong, so blocks are cut short at
        # every row position and the segments roll back again and again.
        scripted = same(home.chat(prompt, tokens, 'auto', script=spoil(plain['token_ids'], 6)),
                        'scripted drafts')
        check_metrics(family, scripted, tokens)
        assert scripted['rounds_per_token'] < 1 and scripted['accepted'] >= scripted['tokens'] // 2, scripted
        # Seeded sampling draws once per kept token, in the same order.
        # (The Qwen3.8 Edge selects greedily only; it has no logits to sample.)
        sampling = ('--temperature', '0.9', '--seed', '7')
        if samples:
            sampled = home.chat(prompt, tokens, 'off', *sampling)
            assert sampled['token_ids'] != plain['token_ids'], 'sampling gate degenerated to greedy'
            for mode, script in (('lookup', None), ('auto', spoil(sampled['token_ids'], 5))):
                again = home.chat(prompt, tokens, mode, *sampling, script=script)
                assert again['token_ids'] == sampled['token_ids'], \
                    f'{family}: drafts changed a seeded sampled reply\n{sampled}\n{again}'
        # A resident conversation: the second turn is prefilled on top of
        # whatever the first left in every segment.
        turns = [(TEXT, 9), (lambda replies: TEXT + replies[0].decode('latin-1') + ' ' + TEXT[:9], 9)]
        off_replies, off_stats, off_log = home.serve('off', turns)
        for mode, script in (('lookup', None), ('auto', [2] * 16)):
            on_replies, on_stats, on_log = home.serve(mode, turns, script=script)
            assert off_replies == on_replies, f'{family}: drafts changed a resident conversation\n{off_replies}\n{on_replies}'
            assert all(drafts_stat(stat) for stat in on_stats), on_stats
            # Rounds that carried one row keep feeding the per-range timing.
            if script:
                for stat in on_stats:
                    fields = stat.split()
                    assert 'STAGES1' in fields and 'LINKS1' in fields, stat
                    at = fields.index('STAGES1')
                    calls = [int(fields[at + 4 + 4 * i]) for i in range(int(fields[at + 1]))]
                    rounds = drafts_stat(stat)
                    assert len(set(calls)) == 1 and 0 < calls[0] <= rounds['rounds'], stat
            assert off_log.count('Segment KV reuse') == on_log.count('Segment KV reuse'), (off_log[-2000:], on_log[-2000:])
        assert all(drafts_stat(stat) is None for stat in off_stats), off_stats
        print(f'{family}: greedy{", seeded" if samples else ""} and resident replies identical with drafts; '
              f'prompt lookup {lookup["rounds"]} rounds for {lookup["tokens"]} tokens '
              f'({lookup["accepted"]}/{lookup["proposed"]} accepted), scripted drafts '
              f'{scripted["rounds"]} rounds for {scripted["tokens"]} tokens '
              f'({scripted["rounds_per_token"]:.3f} rounds/token, {scripted["accepted"]}/{scripted["proposed"]} accepted) PASS',
              flush=True)
        # For the end-of-turn gate: a prompt whose reply starts with printable
        # byte tokens, so the next turn's text re-tokenizes to exactly what
        # the conversation holds, followed by a token that can serve as the
        # checkpoint's end token.
        ending = None
        for shift in range(24):
            text = ''.join(chr(40 + (i * 5 + shift) % 60) for i in range(16))
            reply = home.chat_text(text, 9)
            stops = [i for i in range(1, 5) if all(33 <= t <= 126 for t in reply[:i]) and reply[i] not in reply[:i]]
            if stops:
                ending = (text, reply, stops[-1])
                break
    # One segment that does not offer provisional rows: no drafts anywhere.
    with Household(tmp / 'mixed', family, model_dir, ranges, env, plain=[len(ranges) - 1]) as home:
        mixed = home.chat(prompt, tokens, 'auto', script=plain['token_ids'])
        assert mixed['token_ids'] == plain['token_ids'] and 'draft_metrics' not in mixed, mixed
        print(f'{family}: a chain with one older segment decodes without drafts PASS', flush=True)
    return ending


def check_end_of_turn(tmp, family, model_dir, ranges, env, text, reply, stop):
    """The reply ends inside a block: rows are left over when the turn ends.

    The checkpoint's end token is one the fixture really selects, so the
    first turn stops at reply token `stop` while later rows of the same block
    are still provisional. They must be gone before the second turn, which
    extends the first, is prefilled on top."""
    with Household(tmp / 'eos', family, model_dir, ranges, env) as home:
        turns = [(text, 9), (lambda replies: text + replies[0].decode('latin-1') + ' ' + text[:7], 9)]
        off_replies, off_stats, off_log = home.serve('off', turns)
        on_replies, on_stats, on_log = home.serve('auto', turns, script=reply)
        assert off_replies == on_replies, f'{family}: drafts changed a reply that ends mid-block\n{off_replies}\n{on_replies}'
        assert ' STOP1 eos ' in off_stats[0] and ' STOP1 eos ' in on_stats[0], (off_stats, on_stats)
        first = drafts_stat(on_stats[0])
        assert first and first['discards'] == 1 and first['tokens'] == stop, on_stats
        assert off_log.count('Segment KV reuse') == on_log.count('Segment KV reuse') == 1, (off_log[-2000:], on_log[-2000:])
        assert 'could not discard' not in on_log, on_log[-2000:]
        print(f'{family}: a reply ending mid-block leaves no provisional row behind; '
              f'the next turn reuses the resident state PASS', flush=True)


def olmoe(tmp, env, eos=None):
    import struct
    from gguf_olmoe_fixture import create
    extra = [('tokenizer.ggml.eos_token_id', 4, struct.pack('<I', eos))] if eos is not None else ()
    root = create(tmp / f'olmoe-model-{eos}', layers=2, byte_vocab=True, extra_metadata=extra)
    return root, [(0, 1), (1, 2)], dict(env)


def qwen38(tmp, env, engine, eos=None):
    sys.path.insert(0, str(engine / 'tools'))
    from qwen38_native_gguf_test import fixture, gguf_bytes
    env = {**env, 'Q38_TRUNK_CPU_INT8': '0', 'Q38_TRUNK_CPU_INT4': '0', 'Q38_KV': 'f32', 'GQ_EXACT': '1'}
    root = tmp / f'qwen38-model-{eos}'
    root.mkdir()
    kv, tensors = fixture('float')
    if eos is not None:
        kv = [(k, t, eos if k == 'tokenizer.ggml.eos_token_id' else v) for k, t, v in kv]
    (root / 'model.gguf').write_bytes(gguf_bytes(kv, tensors))
    (root / 'gguf.json').write_text(json.dumps({'format': 'colibri-gguf-v1', 'gguf': 'model.gguf'}))
    meta = subprocess.run([str(ROOT / 'build/test_gguf_inventory'), '--metadata', str(root)], env=env,
                          check=True, capture_output=True, text=True).stdout
    for key, value in json.loads(meta).items():
        (root / (key + '.json')).write_text(json.dumps(value))
    # Layers alternate recurrent (DeltaNet) and attention; both ranges hold one of each.
    return root, [(0, 2), (2, 4)], env


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine', required=True, type=Path)
    parser.add_argument('--only', choices=('olmoe', 'qwen38'))
    args = parser.parse_args()
    os.chdir(ROOT)
    env = {**os.environ, 'OMP_NUM_THREADS': '1', 'COLI_NO_OMP_TUNE': '1', 'PIN': 'off',
           'LUMABRI_ENGINE_BACKEND': 'cpu'}
    env.pop('LUMABRI_DRAFTS', None)
    env.pop('LUMABRI_DRAFT_ROWS', None)
    # A prompt that repeats itself, as quoted text and code do.
    prompt = [5, 9, 12, 7, 5, 9, 12, 7, 5, 9, 12, 7, 5, 9]
    with tempfile.TemporaryDirectory(prefix='lmb-drafts-') as temp:
        tmp = Path(temp)
        for family in ('olmoe', 'qwen38'):
            if args.only and args.only != family:
                continue
            (tmp / family).mkdir()
            build = (lambda eos=None: olmoe(tmp, env, eos)) if family == 'olmoe' else \
                    (lambda eos=None: qwen38(tmp, env, args.engine.resolve(), eos))
            root, ranges, family_env = build()
            for name in ('full', 'mixed', 'eos'):
                (tmp / family / name).mkdir()
            # The OLMoE fixture's own reply repeats itself; the Qwen3.8 one never
            # does, so there lookup proves exact rollback rather than a saving.
            ending = check_family(tmp / family, family, root, ranges, family_env, prompt, 40,
                                  lookup_pays=family == 'olmoe', samples=family == 'olmoe')
            if family == 'qwen38':
                # At the adapter: rollback state and the memory a block may use.
                oracle = subprocess.run([str(ROOT / 'build/draft-room-oracle'), str(root)], env=family_env,
                                        capture_output=True, text=True, timeout=120)
                assert oracle.returncode == 0, oracle.stdout[-2000:] + oracle.stderr[-4000:]
                print(oracle.stdout.strip().splitlines()[-1], flush=True)
                # OLMoE names its end token by a vocabulary string the byte
                # fixture does not have; Qwen3.8 takes it from metadata.
                assert ending, 'no fixture reply starts with printable byte tokens'
                text, reply, stop = ending
                root, ranges, family_env = build(reply[stop])
                check_end_of_turn(tmp / family, family, root, ranges, family_env, text, reply, stop)
    print('DRAFTS ACROSS SEGMENTS: PASS', flush=True)


if __name__ == '__main__':
    main()
