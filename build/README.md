# Package build containers

`build-all` (`build-all.go`, built with `go build` in this directory)
builds the release packages. For every platform in its `platforms` list
it starts a container called `build-<platform>` (for example
`build-ubuntu-2404-amd64`, or `build-fedora-43-amd64` for the
`fedora-43-x86_64` platform), copies the source tarball in over ssh, runs
`./configure && make make-deb` (or `rpmbuild -ta` for Fedora), copies the
packages out to `build-out/`, and stops the container. `build-all -m
upload` then pushes them to the APT and RPM repositories.

    cd build
    go build
    ./build-all ../../aprsc-<version>.tar.gz

Three containers build at a time; `-j <n>` changes that. `-o <regexp>`
builds only the matching platforms, and `-l` lists them. The console
shows the steps of each build prefixed with the platform name, and the
full output of each build goes to `build-logs/<platform>.log`; `-v`
shows it on the console as well. A failed build does not stop the
others. At the end there is a summary, with the last lines of the log of
each failed build, and the exit status is non-zero if any failed. A
container whose build failed is left running for a look inside;
interrupting with ^C kills the running steps and leaves the containers
as they are.

The build host runs LXD. A build container needs:

- a static IPv4 address on the LAN bridge, with a DNS name equal to the
  container name, so that `ssh build-<platform>` works from the build host,
- a user with the same name as the one running `build-all`, with the
  build host user's SSH keys authorised,
- a running SSH server and the packages needed to build aprsc.

## Creating a build container

`lxd-create-builder.sh` creates one from an image that runs cloud-init on
first boot, which sets up the address, the user, the packages and the SSH
server. It picks the package list from the image's OS (Ubuntu or Fedora).
On Ubuntu it finishes by checking the Build-Depends with
`dpkg-checkbuilddeps` against `src/debian/control`. It prints the SSH host
key fingerprints at the end.

1. Pick a free address for the container on the build LAN, and add a DNS
   record for the container name (e.g. `build-ubuntu-2604-amd64`) pointing
   at it.

2. Find the image.

   Ubuntu cloud images come from the `ubuntu:` remote, e.g. for 26.04:

       lxc image list ubuntu: 26.04 amd64 type=container

   Fedora is not on the LXD image server. `images.linuxcontainers.org` has
   it, but only serves its image index to Incus, so
   `lxd-import-image.sh` downloads the newest build of the `cloud`
   variant (the one with cloud-init), checks it against the published
   `SHA256SUMS`, and imports it under the alias `fedora-<release>-cloud`:

       build/lxd-import-image.sh fedora 44

   Imported images do not update themselves. To create a container from a
   newer build later, `lxc image delete fedora-44-cloud` and import again.

3. Create the container. The gateway, name servers and search domain can be
   given with options or set once in the environment:

       export BUILDER_GATEWAY=<gateway> BUILDER_NAMESERVERS=<dns1>,<dns2> BUILDER_SEARCH=<domain>
       build/lxd-create-builder.sh -a <address>/<prefix> build-ubuntu-2604-amd64 ubuntu:26.04
       build/lxd-create-builder.sh -a <address>/<prefix> build-fedora-44-amd64 fedora-44-cloud

   SSH keys are taken from `ssh-add -L` unless `-k <pubkey file>` is given.
   If cloud-init fails, `lxc exec <name> -- cat /var/log/cloud-init-output.log`
   shows why; delete the container with `lxc delete --force <name>` and run
   the script again.

   On Fedora, cloud-init always ends with a "Failed to set the hostname"
   warning, because `hostnamectl` has no D-Bus to talk to in the
   container. The script reports it as status 2 and carries on. The
   hostname is right anyway, since LXD sets it.

4. Log in once to accept the host key, comparing it with the fingerprints
   the script printed:

       ssh build-ubuntu-2604-amd64 true

5. Add the platform to `platforms` in `build-all.go`. Fedora platforms
   also need a `dest` directory in the RPM repository, e.g.
   `dest: "fedora/44/x86_64"`. A trial build of only the new platform:

       cd build && go build && ./build-all -o ubuntu-2604 aprsc-<version>.tar.gz

6. Debian packages are uploaded to the APT repository by distribution
   codename (`make make-deb` takes it from `lsb_release -sc`, `resolute` for
   26.04). Before the first `build-all -m upload`, add a section for the
   new codename to `conf/distributions` of the reprepro repository on the
   distribution host, copying an existing Ubuntu section and changing
   `Codename`. RPMs go to the `dest` directory, which the upload creates.

The container is left running when the script finishes; `build-all`
starts and stops it as needed.

## Notes

- The containers are created with `security.nesting=true` and
  `security.syscalls.intercept.mknod=true`, like the older ones.
- The network configuration goes in the container's `user.network-config`
  key and is applied by cloud-init, so it survives restarts.
  `lxc config show <name>` shows it.
- The Debian containers and the Fedora containers up to 43 were set up by
  hand. Debian could be added to `lxd-create-builder.sh` with the
  `cloud` variant of the linuxcontainers.org image.

## Release announcement

`announcement-template.txt` is the template for the mailing list
announcement. The notes at its top explain the placeholders and how to
summarise the commits since the previous release for server admins.
