# Code signing policy

## Scope and status

This policy applies to official Windows installers published for SaidaEngine
through the [`saias-o/SaidaEngine`](https://github.com/saias-o/SaidaEngine)
repository.

An artifact is a signed SaidaEngine release only when all of the following are
true:

- it was built from this public repository by GitHub Actions on a
  GitHub-hosted runner;
- SignPath verified its repository, workflow and commit origin;
- a project approver manually approved the signing request;
- its Authenticode signature is valid;
- the signed artifact and its SHA-256 digest are published on
  [GitHub Releases](https://github.com/saias-o/SaidaEngine/releases).

Source archives and artifacts explicitly described as unsigned are not covered
by the certificate.

Free code signing provided by [SignPath.io](https://about.signpath.io/), certificate
by [SignPath Foundation](https://signpath.org/).

## Team roles

- Authors (trusted committers): [`@saias-o`](https://github.com/saias-o)
- Reviewers: [`@saias-o`](https://github.com/saias-o)
- Approvers: [`@saias-o`](https://github.com/saias-o)

Changes from people who are not trusted committers must be reviewed before they
are merged. Every release-signing request requires manual approval by an
approver. Anyone added to one of these roles must use multi-factor
authentication for both GitHub and SignPath, and this list must be updated at
the same time.

## Build and signing controls

Release identities are immutable: a tag or published artifact is never moved,
overwritten or rebuilt. A changed artifact requires a new version, commit,
manifest and signing request.

The unsigned artifact is uploaded as a GitHub Actions artifact before it is
submitted through SignPath's GitHub integration. The SignPath project must use
the public repository URL, the GitHub trusted build system and origin
verification. Only artifacts built from source and build scripts maintained in
this repository may be signed.

The artifact configuration must enforce SaidaEngine product naming and a single
consistent product version across the files it signs. After signing, the
release process verifies Authenticode and records the SHA-256 digest of the
signed bytes before publication.

## Privacy policy

This program will not transfer any information to other networked systems unless
specifically requested by the user or the person installing or operating it.

SaidaEngine does not include telemetry or analytics and does not automatically
upload crash reports. Crash logs and minidumps are stored locally.

Network activity occurs only as a consequence of an explicit user or operator
action, including:

- loading a Web build and its project assets from the site the user visits;
- connecting the authoring runtime to an operator-supplied WebSocket endpoint;
- starting and using the operator-controlled MCP endpoint;
- opening the GitHub Sponsors page after the user selects the Sponsor action.

The privacy policy of the chosen hosting service or linked website applies when
the user requests one of those external services. GitHub's handling of data is
described in the
[GitHub Privacy Statement](https://docs.github.com/en/site-policy/privacy-policies/github-general-privacy-statement).

## Reporting

Report suspected policy violations or compromised releases through
[GitHub Issues](https://github.com/saias-o/SaidaEngine/issues). Reports about a
SignPath Foundation signature can also be sent to `support@signpath.io`.
