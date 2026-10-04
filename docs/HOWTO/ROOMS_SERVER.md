# How to run a rooms server

The rooms server lives in its own repository: **https://github.com/MYSOREZ/Generals-Servers**.
Its README is the guide (one-command install on a VPS, options, managing it), and its issues are
the public server list the launcher reads.

Short version, on a Linux VPS as root, with ports 80 and 443 open:

```sh
curl -fsSL https://raw.githubusercontent.com/MYSOREZ/Generals-Servers/main/install.sh | sh -s -- --name "My server"
```

It prints the address (`wss://<ip>.sslip.io/ws`) to enter in the launcher (Rooms → Server), and
to add to the list through the repository's "Add a server" issue form.
