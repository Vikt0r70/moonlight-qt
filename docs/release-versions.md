# Release versions

R1-VERSION: 0.1.30 base=f448ce517ea14159f068081ddcd62455be28019c bump=760c82cfbabb450187c9357faf3df7a3a6a9a928 newest-tag=0.1.29

The R1 customer candidate bump commit sits on the local branch `seathub-r1-bump-0.1.30` (nothing pushed or tagged). Only `app/version.txt` and `SEATHUB_VERSION` in `app/seathub/seathub_version.h` differ from f448ce51, each by one line from 0.1.29 to 0.1.30. Qualified at execution on 2026-10-18: no `seathub-v0.1.30` tag exists and 0.1.30 is newer than the newest published tag `seathub-v0.1.29`.

R2-VERSION: 0.1.31 r1=0.1.30

Chosen at execution on 2026-10-19: 0.1.31 is the next patch above the newest published release, `seathub-v0.1.30` (`gh release list --repo Vikt0r70/moonlight-qt` and `git ls-remote --tags`, which point at commit 760c82cfbabb450187c9357faf3df7a3a6a9a928, the R1 receipt's bump= above), and above the R1 client version 0.1.30 in T013's r1-release.md (`CUSTOMER-SERVED-VERSION: 0.1.30`). No `seathub-v0.1.31` tag exists. Nothing was built, tagged or published here.
