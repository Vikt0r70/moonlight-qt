# Code signing policy

This is the RentaPC client's code signing policy, kept in the public source repository. The same
policy is published for customers at https://rentapc.online/code-signing-policy.

POLICY-STATE: preparation

## Current state

RentaPC client releases are not code signed today. Signing is planned through the free SignPath
Foundation route; no paid certificate route is planned. Nothing in this document claims that
SignPath has accepted the project or that any release is signed. The line above records the
document state: `preparation` until the owner supplies the approval evidence described under
"Policy state" below.

## Planned: free code signing by SignPath Foundation

This section describes what the project intends once, and only if, SignPath approves it. Signing is
planned and not yet in place until SignPath approves the application.

Free code signing provided by SignPath.io, certificate by SignPath Foundation

## What gets signed

- Only binaries that RentaPC builds from the source in this repository: the RentaPC client
  (`SeatHub.exe`) and the RentaPC setup program (`RentaPC-Setup.exe`), as the staged build workflow
  produces them.
- Third-party files are not re-signed under the RentaPC identity. They ship as their upstream
  authors built them. The component list is in [licence-inventory.md](licence-inventory.md).
- Velopack's own helper programs (`Update.exe` and the setup stub) stay unchanged and unsigned
  inside the package. See Q12 below.

## Roles

| Role | Holder | Notes |
|------|--------|-------|
| Author | OWNER-FILL: name of the person who writes and commits release changes | Not yet supplied |
| Reviewer | OWNER-FILL: name of the person who reviews changes before release | Not yet supplied |
| Signing approver | OWNER-FILL: name of the person who approves each production signing request | Not yet supplied |

DEFAULT (pending the owner's review choice):

RentaPC currently has one human maintainer, who holds Author, Reviewer and Signing Approver roles. AI-assisted analysis is advisory and is not represented as independent human approval. External contributions and all changes relative to the signed Moonlight baseline are reviewed and recorded before release. Each production signing request requires the maintainer's explicit approval. Independent review of upstream-engine and release-security changes is recorded when performed; any mandatory independent-review condition will be agreed with SignPath before signing.

No minimum staffing rule is set here. The owner has asked SignPath how it wants the review handled
and has not chosen yet.

## Account security and approval

- Multi-factor authentication is required on every account that can change the source, run the
  release workflow or approve a signing request.
- Every production signing request is approved manually, one release at a time. There is no
  automatic approval.
- Release builds come from the staged GitHub workflow in this repository, never from a developer
  machine.

## Upstream baseline

RentaPC is a fork of moonlight-stream/moonlight-qt 6.1.0. The streaming engine stays unmodified;
the changes are listed in [FORK-CHANGES.md](../FORK-CHANGES.md).

UPSTREAM-SIGNATURE: Valid signer=CN=Cameron Gutman, O=Cameron Gutman, S=Oklahoma, C=US (issuer CN=Sectigo Public Code Signing CA R36) timestamp=Tue Sep 17 06:10:21 2024 (signtool verify /pa /v; countersigned by DigiCert Timestamp 2023) sha256=2537651ee187882ecf68f390f395a7e5005923721800e44b58e9776d3b10578a

That line comes from `Get-AuthenticodeSignature` run on 2026-10-19 against `MoonlightSetup-6.1.0.exe`,
downloaded read-only from the official `v6.1.0` release of moonlight-stream/moonlight-qt on GitHub.
The sha256 is of the downloaded installer. It records what the upstream installer carries; it says
nothing about RentaPC releases.

## Repository name

No repository rename is planned or done in this milestone.

The repository may be renamed in a later milestone (the Internal rename). The questions below
include how SignPath treats a rename after approval.

## Privacy

The client asks, in the RentaPC setup window, whether it may send crash and diagnostic reports.
The notice text is [installer/PRIVACY-NOTICE.txt](../installer/PRIVACY-NOTICE.txt) and every
outbound recipient is listed in [reporting-inventory.md](reporting-inventory.md), including Sentry
for crash and event reports. The full Terms and Privacy Policy are scheduled for a later milestone;
until then the notice links https://rentapc.online/data-collection.

## Open questions for SignPath

Each question stays open until SignPath or the owner answers it. None is claimed as met.

OPEN: Q10 single maintainer: may the owner hold Author/Reviewer/Approver roles, with recorded human self-review and advisory AI analysis? Is independent review specifically required for the Moonlight-derived changes/build scripts, and would a named occasional second reviewer satisfy it?

OPEN: Q11 visible lineage: will the current public GPL repo, retained upstream v6.1.0 history and prominent fork/diff documentation satisfy "visibly uses a fork," despite GitHub `fork:false`? If not, approve the distinct-slug/bridge migration plan before we move.

OPEN: Q12 actual signing inventory/licensing: confirm the Qt LGPL/GPL bundle and own-file allowlist; may unchanged Velopack helpers remain unsigned inside the signed installer/update package? If helpers must be signed, what upstream/derivative provenance and RentaPC metadata are required? Is the proposed staged GitHub-artifact workflow acceptable?

OPEN: Q13 privacy: does the shown bootstrapper notice/choice meet "during installation"? Would installer-launched first-run UI instead be accepted if all optional reporting is blocked beforehand? Confirm the owner's default-ticked design, opt-out warning, full-policy link and versioned re-ask, without claiming any unapproved design is already compliant.

OPEN: Q14 product name: is a product named RentaPC in a repository named moonlight-qt acceptable under SignPath's metadata rule that the product name equals the project name?

OPEN: Q15 later rename: the repository may be renamed in a later milestone (Internal rename). Does the approved project, signing policy and trusted build link survive a rename after approval, or must SignPath be told or the application redone?

## Policy state

`POLICY-STATE: preparation` is the only state this task sets. The state may change to
`signing-approved` only when the owner supplies the `AUTHOR:`, `REVIEWER:` and `APPROVER:` names, a
`SIGNPATH-ACCEPTANCE:` line citing the owner's recorded acceptance evidence, no `OWNER-FILL:` is left,
each of Q10 to Q14 is still open or marked answered with its source, and Q15 is still open.
