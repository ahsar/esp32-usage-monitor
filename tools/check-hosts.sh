#!/usr/bin/env bash
# Check that the trust anchors baked into the firmware still cover every host
# the device talks to.
#
# Why this exists: enabling TLS verification (src/http_client.cpp) means the
# device only trusts the roots in the ESP-IDF bundle that is *compiled into the
# firmware*. That bundle is public-key based (subject name + public key per
# root), so the device keeps working indefinitely as long as each host's chain
# still terminates at a bundled name whose key is unchanged. There is no
# calendar deadline. What does break it — silently, as a STALE panel — is a host
# moving to a CA that predates your frozen bundle, or a CA rotating a root key.
#
# This reads the bundle out of the *built* firmware so it checks what the device
# actually trusts, not what this machine happens to trust.
#
# Usage:  tools/check-hosts.sh [--arch esp32s3]
# Exit:   0 = every host is still covered, 1 = something needs a firmware update.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
arch=""
while [ $# -gt 0 ]; do
  case "$1" in
    --arch) arch="${2:-}"; shift 2 ;;
    *) echo "usage: $0 [--arch <esp32s3|esp32|esp32c6|...>]" >&2; exit 2 ;;
  esac
done

PKG="${HOME}/.platformio/packages/framework-arduinoespressif32-libs"
[ -d "$PKG" ] || { echo "PlatformIO ESP32 libs not found at $PKG" >&2; exit 1; }

# Default to the arch the project's env builds for.
if [ -z "$arch" ]; then
  arch="$(sed -n 's/^[[:space:]]*board_build\.mcu[[:space:]]*=[[:space:]]*//p' \
          "$here/../platformio.ini" | head -1)"
fi
[ -n "$arch" ] || { echo "cannot determine arch; pass --arch" >&2; exit 1; }
lib="$PKG/$arch/lib/libmbedtls.a"
[ -f "$lib" ] || { echo "no libmbedtls.a for arch '$arch' ($lib)" >&2; exit 1; }

exec python3 - "$here/.." "$lib" <<'PY'
import base64, hashlib, os, re, struct, subprocess, sys

root, lib = sys.argv[1], sys.argv[2]

# --------------------------------------------------------------------------
# 1. Pull the CA bundle out of libmbedtls.a
#    ar member "x509_crt_bundle.S.obj" -> .rodata section -> the blob.
#    Blob format (ESP-IDF gen_crt_bundle.py):
#      u16 count, then per entry: u16 name_len, u16 key_len, name(DER), key(DER)
#    The key is a SubjectPublicKeyInfo, not a certificate.
# --------------------------------------------------------------------------
def ar_members(path):
    data = open(path, 'rb').read()
    if data[:8] != b"!<arch>\n":
        sys.exit(f"{path}: not an ar archive")
    off, longnames, out = 8, b"", {}
    while off + 60 <= len(data):
        hdr = data[off:off + 60]
        name = hdr[0:16].decode('latin1').rstrip()
        size = int(hdr[48:58].decode('latin1').strip())
        body = data[off + 60:off + 60 + size]
        off += 60 + size + (size & 1)
        if name == '//':
            longnames = body
            continue
        if name.startswith('/') and name[1:].isdigit():
            i = int(name[1:]); end = longnames.find(b'/\n', i)
            real = longnames[i:end].decode('latin1')
        else:
            real = name.rstrip('/')
        out[real] = body
    return out

def elf_section(blob, prefix):
    e_shoff, = struct.unpack_from('<I', blob, 0x20)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', blob, 0x2E)
    def sh(i):
        return struct.unpack_from('<IIIIIIIIII', blob, e_shoff + i * e_shentsize)
    st = sh(e_shstrndx); strtab = blob[st[4]:st[4] + st[5]]
    for i in range(e_shnum):
        s = sh(i)
        if strtab[s[0]:].split(b'\0')[0].decode().startswith(prefix):
            return blob[s[4]:s[4] + s[5]]
    return None

def common_name(dn_der):
    out = subprocess.run(["openssl", "asn1parse", "-inform", "DER"],
                         input=dn_der, capture_output=True).stdout.decode()
    lines = out.splitlines()
    cn = ""
    for i, line in enumerate(lines):
        if "commonName" in line and i + 1 < len(lines):
            m = re.search(r':\s*([^:]+)$', lines[i + 1])
            if m:
                cn = m.group(1).strip()
    return cn

def bundle_entries(blob):
    count, = struct.unpack_from('>H', blob, 0)
    off, out = 2, {}
    for _ in range(count):
        name_len, key_len = struct.unpack_from('>HH', blob, off); off += 4
        name = blob[off:off + name_len]; off += name_len
        key = blob[off:off + key_len]; off += key_len
        pem = ("-----BEGIN PUBLIC KEY-----\n" +
               "\n".join(base64.encodebytes(key).decode().split()) +
               "\n-----END PUBLIC KEY-----\n").encode()
        der = subprocess.run(["openssl", "pkey", "-pubin", "-outform", "DER"],
                             input=pem, capture_output=True).stdout
        out[common_name(name)] = hashlib.sha256(der).hexdigest()
    return out, off, count

member = next((b for n, b in ar_members(lib).items() if 'x509_crt_bundle' in n), None)
if member is None:
    sys.exit(f"{lib}: no x509_crt_bundle member (built without the CA bundle?)")
blob = elf_section(member, ".rodata")
if blob is None:
    sys.exit(f"{lib}: bundle section not found")
bundle, consumed, entries = bundle_entries(blob)

# --------------------------------------------------------------------------
# 2. Which hosts does the firmware actually talk to?
# --------------------------------------------------------------------------
hosts = set()
for fn in ("src/api_client.cpp", "src/weather_client.cpp", "src/http_client.cpp"):
    p = os.path.join(root, fn)
    if not os.path.exists(p):
        continue
    for line in open(p):
        if re.search(r'\b[A-Z_]*HOST\b\s*=', line):
            for h in re.findall(r'"([A-Za-z0-9.-]+\.[A-Za-z]{2,})"', line):
                hosts.add(h)
hosts = sorted(hosts)

# --------------------------------------------------------------------------
# 3. For each host: does its chain still end at a bundled anchor?
# --------------------------------------------------------------------------
def chain_pems(host):
    out = subprocess.run(["openssl", "s_client", "-connect", f"{host}:443",
                          "-servername", host, "-showcerts"],
                         input=b"", capture_output=True).stdout.decode()
    return re.findall(r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----",
                      out, re.S)

def x509(pem, *args):
    return subprocess.run(["openssl", "x509", "-noout", *args],
                          input=pem.encode(), capture_output=True).stdout.decode().strip()

def field(text, key):
    m = re.search(rf'{key}=(.*)', text)
    return m.group(1).strip() if m else ""

def cn_of(dn):
    # "C=US, O=Acme, CN=Foo" -> Foo
    parts = [p.strip() for p in dn.split(",")]
    for p in reversed(parts):
        if p.startswith("CN="):
            return p[3:]
    return dn

print(f"arch {os.path.basename(os.path.dirname(os.path.dirname(lib)))}: "
      f"bundle has {entries} anchors ({consumed} bytes) — "
      f"verified from the built library, not this machine's trust store")
print()
print(f"{'host':22} {'status':8} matched anchor (issuer found in the bundle)")
print("-" * 72)

failed = []
for host in hosts:
    pems = chain_pems(host)
    if not pems:
        print(f"{host:22} {'UNREACHABLE':8} cannot connect")
        failed.append(host)
        continue
    # esp_crt_bundle is consulted per certificate: it looks the *issuer* of the
    # cert it is trying to verify up in the bundle and, on a hit, validates the
    # signature against the stored key and stops. So walk leaf -> up and take the
    # first issuer we recognise; the top cert need not be a root at all
    # (wttr.in sends a cross-signed "Root YR" whose issuer is ISRG Root X1).
    matched = ""
    for pem in pems:
        issuer = cn_of(field(x509(pem, "-issuer"), "issuer"))
        if issuer in bundle:
            matched = issuer
            break
    if matched:
        print(f"{host:22} {'ok':8} {matched}")
    else:
        issuers = [cn_of(field(x509(p, "-issuer"), "issuer")) for p in pems]
        print(f"{host:22} {'NOT COVERED':8} chain issuers {issuers} — none bundled")
        failed.append(host)

print()
if failed:
    print(f"{len(failed)} host(s) need attention: {', '.join(failed)}")
    print("The device will fail TLS to these until the firmware is rebuilt with a")
    print("newer Arduino/IDF release (which carries a newer CA bundle) and reflashed.")
    sys.exit(1)
print(f"all {len(hosts)} hosts still chain to an anchor in the device's bundle")
PY
