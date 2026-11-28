# AGENTS.md

These rules bind everyone who changes this repository, people and AI agents alike. Read [Docs/TESTING.md](Docs/TESTING.md) before you change anything. It says how a bug is shown, how the build and the tests run, and what has to be true before a commit. These rules count for small changes and obvious bugs too. If a task brief says otherwise, these rules still apply: stop and say so.

## Every piece of work

1. **Evidence first.** A bug exists when a test fails on the current code, or a measurement shows it. Reading code and concluding it is wrong gives a hypothesis, nothing more.
2. **No fix without a failing test.** The order is: write the test, see it fail for the stated reason, fix, see it pass. A fix written before its test failed is out of order.
3. **Positive claims need proof too.** "Faster", "no longer allocates", "cannot race" and "safe" are hypotheses until a test or a measurement shows them.
4. **Never weaken a test to get green.** Do not loosen, skip, narrow or delete a failing check. A test that fails when you did not expect it is a finding: find the mechanism.
5. **Fix only what the evidence shows.** Do not widen a fix into hardening, refactoring or a redesign because the code nearby looks suspicious.
6. **Nothing is swept under the rug.** A suspicion you cannot show is neither fixed nor dropped. Write it down where the maintainer sees it (the bug hunt list in `Reports/BugHunts`, or an issue): what it is, the exact conditions, and how it could be shown.
7. **Label claims.** Say what you observed in a run, what you read in the code, and what you assume. Never present an unobserved result as fact.

## Keep fixes small

- Make the smallest change that turns the failing test green, in the existing structure and style. Prefer adding or correcting a line inside the existing code over replacing it.
- **Budget:** if a fix needs more than about 30 changed lines outside `Tests`, or touches more than two source files, stop before writing it. Describe the evidence, the smallest change and the alternatives, and let the maintainer decide. Test code may be longer, but it should also be the smallest test that shows the bug.
- Do not rename, move or reorganise code, add abstractions, or clean up "while you are there". If you cannot say why a line had to change for the failure you showed, put it back.
- If the only correct fix changes the design (ownership, lifetimes, threading, startup order, new mechanisms, or behaviour the user sees), stop and let the maintainer decide.
- Every change must be explainable in a few sentences: what was shown to be wrong, what changed, and what was left alone.

## Every fix is reviewed

- After a fix is green, someone other than its author reviews it and looks for bugs in the fix itself. They check every caller of a changed function, edge cases, 32-bit versus 64-bit, threading and lifetimes.
- They also mentally take the fix out: would the test still fail, for the stated reason, every time?
- A problem the review finds is handled under the same rules: a test that shows it, then the correction. A fix that cannot be corrected this way is reverted, and the bug goes back on the list.

## Directing other agents

- Put these rules and the pointer to `Docs/TESTING.md` into every brief you write for another agent.
- Before you hand out a task, ask what evidence exists for it. Without evidence, the task is to gather it (a test, a probe, a measurement), not to fix.
- Check the agent's work against these rules before you accept it, and have the review done by someone other than the agent that wrote the fix.

## Working in this repository

- **Worktrees:** other sessions work in the main checkout at the same time. Do your work in your own git worktree. Before each commit in a shared checkout, look at the staged hunks of every file.
- **No pushing** unless the maintainer asks for it. Merging into `main` also needs the maintainer's word.
- **Commits:** one commit per shown bug, with the test and the fix together. Use the style of the history: `<Area>: <what is now true>`. Commits written with an AI assistant end with a `Co-Authored-By:` line.
- **Public repository:** everything committed, and every issue or comment, is in English. Do not name games, local paths, user names or e-mail addresses. Use neutral wording such as "a 32-bit game" or "a real Cheat Engine table".
- **Style:** keep the style of the file. Do not add comments that narrate the change. A reason the code cannot show goes into a `∑` rationale comment, as elsewhere in the code.
- **The maintainer's machine:** never run tests while a game runs (see `Docs/TESTING.md`). Never attach to, inject into or write to a process other than the test programs of your own build. Never delete files you did not create. Never touch `Install` or the user's settings and home folders.
