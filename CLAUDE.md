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
