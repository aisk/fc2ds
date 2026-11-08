# Development environment setup

The NDS toolchain runs in WSL (Debian). Python tools run on Windows or in WSL.

## 1. Base packages (WSL Debian, as root)

```sh
apt update
apt install -y build-essential git wget ca-certificates cc65 python3 python3-pil
```

## 2. BlocksDS (WSL Debian, as root)

devkitPro's servers return HTTP 403 from some networks, so we use BlocksDS on the Wonderful toolchain.

```sh
mkdir -p /opt/wonderful && cd /opt/wonderful
wget https://wonderful.asie.pl/bootstrap/wf-bootstrap-x86_64.tar.gz
tar xzf wf-bootstrap-x86_64.tar.gz && rm wf-bootstrap-x86_64.tar.gz
export PATH=/opt/wonderful/bin:$PATH
wf-pacman -Syu --noconfirm wf-tools
wf-config repo enable blocksds
wf-pacman -Syu --noconfirm
wf-pacman -S --noconfirm blocksds-toolchain
ln -sfn /opt/wonderful/thirdparty/blocksds /opt/blocksds

cat > /etc/profile.d/blocksds.sh <<'EOF'
export WONDERFUL_TOOLCHAIN=/opt/wonderful
export BLOCKSDS=/opt/blocksds/core
export BLOCKSDSEXT=/opt/blocksds/external
export PATH=/opt/wonderful/bin:/opt/wonderful/toolchain/gcc-arm-none-eabi/bin:$PATH
EOF
```

When piping a script from PowerShell, strip CR first, or bash fails on `\r`:

```powershell
(Get-Content script.sh -Raw) -replace "`r","" | wsl -d Debian -u root -- bash -s
```

## 3. Verify

```sh
bash -lc 'arm-none-eabi-gcc --version; cat $BLOCKSDS/version.txt'
```

`make verify` also needs a host C compiler (`gcc`) and Python 3 with PyYAML.

## 4. Headless NDS checks (optional)

`common/tools/dscheck.py` runs a build in DeSmuME without a window (screenshots, scripted input, CPU load). It needs py-desmume, which installs without root through [uv](https://docs.astral.sh/uv/):

```sh
uv venv --python 3.12 ~/.local/share/fc2ds/dsenv
uv pip install --python ~/.local/share/fc2ds/dsenv/bin/python py-desmume pillow
~/.local/share/fc2ds/dsenv/bin/python common/tools/dscheck.py --help
```

## 5. Notes

- The project lives inside WSL (Debian testing). Automatic mounting of Windows drives is off on purpose, so do not rely on `/mnt/c`. Windows tools reach the files through `\\wsl.localhost\Debian\home\<user>\fc2ds\...`.
- devkitPro's package servers return HTTP 403 from this network; do not try to install it.
- Passwordless sudo is not guaranteed; ask the user to run commands that need root.

## 6. Windows-side tools

- Mesen 2: debugging the original game
- melonDS: running and debugging the NDS build
- Python 3 + Pillow: `pip install pillow`
