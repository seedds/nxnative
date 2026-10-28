# Build an iOS IPA from Linux

Linux cannot compile NxNative's iOS app or Metal shaders. This workflow sends an encrypted bundle of a game's `.nsp`, `.xci`, `.nsz`, or `.xcz` files and `prod.keys` through your private S3 bucket to a remote Apple Silicon Mac running macOS 26.6 and Xcode 27.1. The build runs NxNative's existing packer and downloads the unsigned IPA beside the local game files.

The game and keys are encrypted locally with `age` and again at rest in S3. The Bitrise worker decrypts them while building. They are not uploaded to GitHub, but they do leave your Linux computer and are accessible to administrators of the Bitrise project and AWS account. This workflow uses private S3 objects for both input and output; configure lifecycle expiry to remove them automatically.

## One-time setup

1. This repository is already a fork of `seedds/nxnative`, with the Linux workflow committed on `main`. GitHub keeps forks of this public upstream public, so the fork's source code and history are visible to everyone. The workflow keeps ROMs, keys, archives, and IPAs out of GitHub. For private source, create a separate private repository instead of a fork. Preserve the repository's `LICENSE` and third-party license files.
2. Connect the Bitrise project to `well0l/nxnative` with the GitHub App and grant it access to this repository only. GitHub delivers push and tag events through the app, so no manual webhook is needed. Store the Bitrise configuration in the repository: in Workflow Editor choose **Change storage… → Git repository**, and leave the path empty for the root `bitrise.yml` on `main`. The workflow uses the `osx-xcode-27.1.x-edge` Apple Silicon stack. This is an edge preview; its published report lists Xcode 27.1, macOS 26.6.1, the iOS SDK, an installed Metal Toolchain, and about 107 GB of free disk. Check current Bitrise availability and pricing before relying on it.
3. Create the private S3 bucket and two narrowly scoped IAM users from [`deploy/bitrise-s3.yaml`](../deploy/bitrise-s3.yaml). For example, with AWS CLI credentials that can create CloudFormation and IAM resources:

   ```sh
   aws cloudformation deploy \
     --template-file deploy/bitrise-s3.yaml \
     --stack-name nxnative-build-storage \
     --capabilities CAPABILITY_IAM
   ```

   The stack blocks public access, applies S3-managed encryption, expires objects under `nxnative/` after seven days, and creates separate Linux and Bitrise users. It does not create access keys. Create one key for each user in AWS IAM; use the Linux key only on Linux and put the Bitrise key in Bitrise project secrets. Set the stack's `BucketName` output as `NX_S3_BUCKET` and use the AWS region where you deployed it as `AWS_DEFAULT_REGION`. Do not put AWS credentials in the repository.
4. On Linux, install AWS CLI, `age`, Python 3.12+, and Git. On Arch Linux, install the official packages with `sudo pacman -S --needed aws-cli-v2 age`; use your distribution's official packages on other systems. Configure the AWS CLI locally with the Linux user's credentials, for example with `aws configure`; do not put them in the repository or this chat. This Bitrise project already has a matching `age` identity registered as the protected `NX_AGE_IDENTITY` secret. Use its public recipient as `NX_AGE_RECIPIENT`; do not generate a replacement unless you also update the Bitrise secret. For a fresh Bitrise setup, generate an identity outside the repository and add it as the protected `NX_AGE_IDENTITY` secret:

   ```sh
   mkdir -p "$HOME/.config/nxnative"
   chmod 700 "$HOME/.config/nxnative"
   age-keygen -o "$HOME/.config/nxnative/age-identity.txt"
   ```

   Add `NX_S3_BUCKET` and `AWS_DEFAULT_REGION` as Bitrise environment variables; add `AWS_ACCESS_KEY_ID` and `AWS_SECRET_ACCESS_KEY` as project secrets. For a fresh setup, read the `# public key:` recipient from the identity file and set it on Linux as `NX_AGE_RECIPIENT`.
5. Confirm that Bitrise uses the repository-stored `bitrise.yml`, that the `nxnative-cloud-build` tag trigger is enabled, and that `origin` points to `https://github.com/well0l/nxnative.git`. Do not create an AWS access key with broader permissions than the template grants.

## Build a game

From the NxNative checkout on Linux:

```sh
export NX_S3_BUCKET='your-private-bucket'
export NX_AGE_RECIPIENT='age1…'
scripts/build_ipa_linux.sh '/path/to/game.nsp' --keys '/path/to/prod.keys'
```

Pass one `.nsp`, `.xci`, `.nsz`, or `.xcz` file, or a folder containing the game's package files when it has updates or DLC. The command creates an encrypted temporary archive beside the input, uploads it to S3, pushes a one-use tag to `origin`, waits for Bitrise, and saves the IPA beside the input. The temporary local archive is removed when the command exits. Input and output objects remain in S3 until the lifecycle rule expires them. Build errors appear in the Bitrise log for the tag printed by the command.

The result is an unsigned IPA, as produced by NxNative on a Mac. Install it on your iPhone using AltStore, Sideloadly, or another sideloading tool. Your iPhone must meet NxNative's supported OS and hardware requirements. Bitrise and AWS accounts, build minutes, storage, bandwidth, and possible charges are user-provided.

## Limits

- Every title is different. NxNative can survey unknown titles and attempt a provisional profile, but a generated IPA is not a guarantee that a game will boot or run correctly.
- Some builds capture shaders by running the game's arm64 code in NxNative's Mac host. The workflow uses an Apple Silicon worker and runs that host headlessly; a title whose capture step fails will stop the build.
- The first build for a title can use substantial disk space and time. The upstream packer estimates about twice the game size plus 10 GB; the selected Bitrise stack reports about 107 GB free at present.
- This workflow does not sign the IPA for installation. You still need to sideload it with an Apple account and signing method of your choice.
