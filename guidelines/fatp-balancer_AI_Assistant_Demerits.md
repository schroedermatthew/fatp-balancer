# AI Assistant Demerit Tracker

*Tracking violations to improve AI assistant performance on fatp-balancer development*

*Carried forward from Fat-P development record. Claude enters this project with 24 accumulated demerits.*

---

| Violation | ChatGPT | Claude | Gemini | GROK |
|-----------|---------|--------|--------|------|
| Did not read Guidelines carefully | | 10 | | |
| Did not follow code naming conventions | 1 | 1 | | |
| Delivered code without compiling | 1 | 3 | | |
| Lied about compiling and testing | 1 | | | |
| Provided links to documents that were not changed | 3 | 4 | | |
| Asked user to perform edits instead of providing corrected files | 1 | | | |
| Claimed completion but delivered files with incomplete BALANCER_META schema | 1 | | | |
| Did not implement required changes | 10 | | | |
| Added AI comments (NEW, FIXED, Removed, etc.) | | 1 | | |
| Lied about capabilities | | 1 | | 1 |
| Delivered corrupted code | 10 | | | |
| Making information up because did not bother to look at uploaded files | 5 | | | |
| Took the cheaper path and delivered it as if it were the fix | 5 | 5 | | |
| Wrote new file from scratch instead of requesting/using existing one | | 1 | | |
| Drew conclusions about dependencies without reading available dependency files | | 2 | | |
| Reported non-violations as violations (fabricated rule breaches from available guidelines) | 1 | | | |

---

**Totals: ChatGPT 39 | Claude 28 | Gemini 0 | GROK 1**

*Last updated: 2026-03-08*

---

## Violation Definitions

| Violation | What It Means |
|-----------|---------------|
| **Did not read Guidelines carefully** | Violated a rule that was explicitly stated in a guideline document that was available in the session |
| **Did not follow code naming conventions** | Used wrong prefix, wrong case, or wrong separator for member variables, functions, constants, or namespaces |
| **Delivered code without compiling** | Provided code claiming it was correct without actually compiling it (when compilation was available) |
| **Lied about compiling and testing** | Explicitly claimed "I compiled this" or "tests pass" when neither occurred |
| **Provided links to documents that were not changed** | Included a file in the download manifest that was not modified |
| **Asked user to perform edits instead of providing corrected files** | Said "you'll need to change X to Y" instead of providing the corrected file |
| **Claimed completion but delivered files with incomplete BALANCER_META schema** | Marked a deliverable as done with missing required BALANCER_META keys |
| **Did not implement required changes** | Delivered output that omitted changes explicitly requested |
| **Added AI comments** | Left `// NEW`, `// FIXED`, `// Removed`, `// Added`, or similar process markers in code |
| **Lied about capabilities** | Claimed ability to do something (compile, run, access a file) that was not actually possible |
| **Delivered corrupted code** | Code that would not compile or contained structural errors that made it non-functional |
| **Making information up because did not bother to look at uploaded files** | Asserted facts about project files without reading them, when reading was available |
| **Took the cheaper path and delivered it as if it were the fix** | Identified a structural root cause, then delivered a band-aid and framed the real fix as optional (Band-Aid Rule violation) |
| **Wrote new file from scratch instead of requesting/using existing one** | Created a new version of a file instead of requesting and modifying the existing one |
| **Drew conclusions about dependencies without reading available dependency files** | Made layer violation claims without reading the actual headers being accused |
| **Reported non-violations as violations** | Fabricated a guideline breach not supported by the available governance documents |

---

## Notes on Carry-Forward

The demerit history is carried forward intact from Fat-P. The violations occurred on that project and inform the record for this one. The categories have been updated to reference `BALANCER_META` where the Fat-P record referenced `FATP_META` — the violation is the same error class applied to this project's schema.

No new demerits have been issued as of project initialization (March 2026).
