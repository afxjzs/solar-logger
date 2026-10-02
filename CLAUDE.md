# BMW Solar Logger — working rules

## ABUD — Always Be Updating Documentation

The canonical docs in `docs/` are the engineering record, not chat history. Update them in the same work as the code, measurement, correction, or decision they describe. See `docs/INDEX.md` for which document owns what.

## DTYM — Don't Trust Your Memory

Read the docs and the code before asserting anything about this project. Doug has said explicitly that he will spend the tokens. This extends to datasheets: verify register values and bit fields against the actual TI document rather than recalling them.

## End each major stint with an orchestrator agent report

Work on this project is coordinated through an orchestrator agent. At the end of every major stint, print a short, concise report as markdown **in a single code block** in the chat, so Doug can copy it into his reply to the orchestrator.

Keep it to what the orchestrator needs to act:

- status (built / compiled / uploaded / measured), stated plainly
- what changed, in one or two lines
- facts that were verified, and what they were verified against
- what to run next, as concrete commands
- expected output, with anything predicted-but-unobserved labeled as such
- what was left unchanged

Predictions and measurements must never be formatted alike.

## CODE kickoff prompts go in a single code block

When the orchestrator writes a kickoff for a fresh CODE session, print the whole prompt as markdown **in a single fenced code block** in the chat, so Doug can copy it in one action. For this project that replaces the global `PROMPT:` / `---` envelope; the reason is the same either way, which is that the prompt gets copied somewhere else, so its boundaries have to be unmistakable and the copy has to be one gesture.

Nothing but the prompt goes inside the fence. Commentary, caveats and open questions go before or after it, never inside. If the prompt's own text needs a fenced block, use a longer outer fence (````) so the inner one cannot close it.
