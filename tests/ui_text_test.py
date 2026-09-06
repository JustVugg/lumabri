"""Regression guard for English application copy, not user/model output.

This is a targeted vocabulary check, not a general language detector. Comments,
test fixtures and text streamed by a model may legitimately use any language.
"""
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parent.parent
SOURCES = (
    "lumabri.c", "src/ui/lumabri_tui.c", "src/ui/lumabri_home_ui.h", "src/ui/lumabri_chat_editor.h",
    "lumabri_home_runtime.h", "lumabri_home.h", "lumabri_client.h",
)
# Match comments and character literals too, so quoted text inside them is
# never mistaken for an application string.
TOKENS = re.compile(r'/\*.*?\*/|//[^\n]*|\'(?:\\.|[^\'\\])*\'|"(?:\\.|[^"\\])*"', re.S)
ITALIAN = re.compile(
    r"\b(?:comandi|comando|sciame|modello|modelli|scegli|nessun|nessuna|"
    r"motore|esperti|donazione|pronto|pronta|residenti|incompleta|"
    r"invio|annulla|attesa|elaborazione|inferenza|conversazione|"
    r"disponibili|fette|saltato|indirizzo|ripristino|riapro|protetti|"
    r"chiamate|ancora|interrompo|ricordato|serviti|sottocartella)\b",
    re.I,
)


def main():
    failures = []
    for relative in SOURCES:
        source = (ROOT / relative).read_text()
        for token in TOKENS.finditer(source):
            if not token[0].startswith('"'):
                continue
            found = ITALIAN.search(token[0])
            if found:
                line = source.count("\n", 0, token.start()) + 1
                failures.append(f"{relative}:{line}: old Italian UI copy: {found[0]}")
    assert not failures, "\n".join(failures)
    help_text = subprocess.check_output([str(ROOT / "test_chat_ui"), "help"], text=True)
    for command in ("/swarm", "/experts", "/hosts", "/model", "/debug", "/storage", "/reset", "/quit"):
        assert command in help_text, f"missing help for {command}"
    for phrase in ("Tab completes commands", "new conversation", "close chat", "hosted: current model only"):
        assert phrase in help_text, f"missing English help: {phrase}"
    print("UI TEXT: PASS (application copy and rendered command help)")


if __name__ == "__main__":
    main()
