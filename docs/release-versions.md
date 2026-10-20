# Release versions

R1-VERSION: 0.1.30 base=f448ce517ea14159f068081ddcd62455be28019c bump=760c82cfbabb450187c9357faf3df7a3a6a9a928 newest-tag=0.1.29

The R1 customer candidate bump commit sits on the local branch `seathub-r1-bump-0.1.30` (nothing pushed or tagged). Only `app/version.txt` and `SEATHUB_VERSION` in `app/seathub/seathub_version.h` differ from f448ce51, each by one line from 0.1.29 to 0.1.30. Qualified at execution on 2026-10-18: no `seathub-v0.1.30` tag exists and 0.1.30 is newer than the newest published tag `seathub-v0.1.29`.

R2-VERSION: 0.1.31 r1=0.1.30

Chosen at execution on 2026-10-19: 0.1.31 is the next patch above the newest published release, `seathub-v0.1.30` (`gh release list --repo Vikt0r70/moonlight-qt` and `git ls-remote --tags`, which point at commit 760c82cfbabb450187c9357faf3df7a3a6a9a928, the R1 receipt's bump= above), and above the R1 client version 0.1.30 in T013's r1-release.md (`CUSTOMER-SERVED-VERSION: 0.1.30`). No `seathub-v0.1.31` tag exists. Nothing was built, tagged or published here.

R2-REPAIR-VERSION: 0.1.32 prior=0.1.31 r1=0.1.30

Chosen at execution on 2026-10-19 by T083, the Setup-file description repair (the published `RentaPC-Setup.exe` described itself as `RentaPC setup` where ADR-0092 inventory row 53 approves `RentaPC`). 0.1.32 is the next patch above the newest published release, `seathub-v0.1.31` (`git ls-remote --tags origin "seathub-v0.1.*"` newest tag, and `gh release list --repo Vikt0r70/moonlight-qt` lists SeatHub 0.1.31 as Latest, published 2026-10-19T16:43:12Z), and above the R1 client version 0.1.30. No `seathub-v0.1.32` tag exists locally or on origin. The R2-VERSION record above is kept as it was: 0.1.31 was the first R2 candidate and is already published, so this record is the repair version, not a replacement. Nothing was built, tagged or published by this record.

R3-CLIENT-VERSION: 0.1.33 prior=0.1.32 r1=0.1.30

Chosen at execution on 2026-10-20 by T090, the binary-safe pairing key (ADR-0109). Remote floor observed read-only just before the bump: `gh release list --repo Vikt0r70/moonlight-qt` lists SeatHub 0.1.32 as Latest (tag `seathub-v0.1.32`, published 2026-10-19T22:05:12Z), and `git ls-remote --tags origin "seathub-v0.1.3*"` lists `seathub-v0.1.30`, `seathub-v0.1.31` and `seathub-v0.1.32` only. 0.1.33 is the next patch above that newest published release and above the R1 client version 0.1.30. No `seathub-v0.1.33` tag exists locally or on origin. The R2-VERSION and R2-REPAIR-VERSION records above are kept as they were. Nothing was built, tagged or published by this record.
