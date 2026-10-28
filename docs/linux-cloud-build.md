# Build an iOS IPA from Linux

Linux cannot compile NxNative's iOS app or Metal shaders. This workflow sends an encrypted bundle of a game's `.nsp`, `.xci`, `.nsz`, or `.xcz` files and `prod.keys` through your private S3 bucket to a remote Apple Silicon Mac running macOS 26.6 and Xcode 27.1. The build runs NxNative's existing packer and downloads the unsigned IPA beside the local game files.

The game and keys are encrypted locally with `age` and again at rest in S3. The Bitrise worker decrypts them while building. They are not uploaded to GitHub, but they do leave your Linux computer and are accessible to administrators of the Bitrise project and AWS account. This workflow uses private S3 objects for both input and output; configure lifecycle expiry to remove them automatically.

## One-time setup

1. This repository is already a fork of `seedds/nxnative`, with the Linux workflow committed on `main`. GitHub keeps forks of this public upstream public, so the fork's source code and history are visible to everyone. The workflow keeps ROMs, keys, archives, and IPAs out of GitHub. For private source, create a separate private repository instead of a fork. Preserve the repository's `LICENSE` and third-party license files.
2. Create a Bitrise project connected to `well0l/nxnative`. In Bitrise's Workflow Editor, set the configuration YAML source to the `bitrise.yml` stored on the repository's `main` branch; new Bitrise apps can keep their config on Bitrise by default. The file defines a tag-triggered workflow on the `osx-xcode-27.1.x-edge` Apple Silicon stack. The stack is an edge preview, and its currently published report lists Xcode 27.1, macOS 26.6.1, the iOS SDK, an installed Metal Toolchain, and about 107 GB of free disk. Check the current Bitrise availability and pricing before relying on it.
3. Create an S3 bucket with Block Public Access enabled. Keep the Linux AWS credentials and Bitrise AWS credentials separate. Give Linux permission to write only `nxnative/incoming/*` and read `nxnative/status/*` plus `nxnative/outgoing/*`. Give Bitrise permission to read only `nxnative/incoming/*` and write `nxnative/status/*` plus `nxnative/outgoing/*`. Set an S3 lifecycle rule to expire all objects under `nxnative/`. Do not put AWS credentials in the repository.
4. On Linux, install AWS CLI, `age`, Python 3.12+, and Git. Configure the AWS CLI with the Linux credentials. Store the `age` identity outside the repository and create one:

   ```sh
   mkdir -p "$HOME/.config/nxnative"
   chmod 700 "$HOME/.config/nxnative"
   age-keygen -o "$HOME/.config/nxnative/age-identity.txt"
   ```

   Add the complete identity file as the Bitrise secret `NX_AGE_IDENTITY`. Add `NX_S3_BUCKET`, `AWS_DEFAULT_REGION`, `AWS_ACCESS_KEY_ID`, and `AWS_SECRET_ACCESS_KEY` to the Bitrise project; mark the AWS key values and identity as secret. Read the `# public key:` recipient from the identity file and set it on Linux as `NX_AGE_RECIPIENT`.
5. Confirm that Bitrise uses the repository-stored `bitrise.yml`, that the `nxnative-cloud-build` tag trigger is enabled, and that `origin` points to `https://github.com/well0l/nxnative.git`. Do not create an AWS access key with broader permissions than these prefixes require.

## Build a game

From the NxNative checkout on Linux:

```sh
export NX_S3_BUCKET='your-private-bucket'
export NX_AGE_RECIPIENT='age1…'
scripts/build_ipa_linux.sh '/path/to/game NSP folder' --keys '/path/to/prod.keys'
```

The command creates an encrypted temporary archive beside the game's NSPs, uploads it to S3, pushes a one-use tag to `origin`, waits for Bitrise, and saves the IPA next to the NSPs. The temporary local archive is removed when the command exits. Input and output objects remain in S3 until the lifecycle rule expires them. Build errors appear in the Bitrise log for the tag printed by the command.

The result is an unsigned IPA, as produced by NxNative on a Mac. Install it on your iPhone using AltStore, Sideloadly, or another sideloading tool. Your iPhone must meet NxNative's supported OS and hardware requirements. Bitrise and AWS accounts, build minutes, storage, bandwidth, and possible charges are user-provided.

## Limits

- Every title is different. NxNative can survey unknown titles and attempt a provisional profile, but a generated IPA is not a guarantee that a game will boot or run correctly.
- Some builds capture shaders by running the game's arm64 code in NxNative's Mac host. The workflow uses an Apple Silicon worker and runs that host headlessly; a title whose capture step fails will stop the build.
- The first build for a title can use substantial disk space and time. The upstream packer estimates about twice the game size plus 10 GB; the selected Bitrise stack reports about 107 GB free at present.
- This workflow does not sign the IPA for installation. You still need to sideload it with an Apple account and signing method of your choice.
