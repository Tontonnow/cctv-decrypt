from __future__ import annotations
import base64
import json
import os
import ssl
import urllib.request
MASK32 = 0xFFFFFFFF


def _rol(x, n):
    n &= 31
    return ((x << n) | (x >> (32 - n))) & MASK32


# ---------------------------------------------------------------- SM3
_SM3_IV = [0x7380166F, 0x4914B2B9, 0x172442D7, 0xDA8A0600,
           0xA96F30BC, 0x163138AA, 0xE38DEE4D, 0xB0FB0E4E]


def _p0(x):
    return x ^ _rol(x, 9) ^ _rol(x, 17)


def _p1(x):
    return x ^ _rol(x, 15) ^ _rol(x, 23)


def _sm3_compress(v, block):
    w = list(int.from_bytes(block[i * 4:i * 4 + 4], 'big') for i in range(16))
    for j in range(16, 68):
        w.append(_p1(w[j - 16] ^ w[j - 9] ^ _rol(w[j - 3], 15))
                 ^ _rol(w[j - 13], 7) ^ w[j - 6])
    w1 = [w[j] ^ w[j + 4] for j in range(64)]
    a, b, c, d, e, f, g, h = v
    for j in range(64):
        t = 0x79CC4519 if j < 16 else 0x7A879D8A
        ss1 = _rol((_rol(a, 12) + e + _rol(t, j)) & MASK32, 7)
        ss2 = ss1 ^ _rol(a, 12)
        if j < 16:
            ff = a ^ b ^ c
            gg = e ^ f ^ g
        else:
            ff = (a & b) | (a & c) | (b & c)
            gg = (e & f) | ((~e & MASK32) & g)
        tt1 = (ff + d + ss2 + w1[j]) & MASK32
        tt2 = (gg + h + ss1 + w[j]) & MASK32
        d, c, b, a = c, _rol(b, 9), a, tt1
        h, g, f, e = g, _rol(f, 19), e, _p0(tt2)
    return [x ^ y for x, y in zip(v, [a, b, c, d, e, f, g, h])]


def sm3(data):
    v = list(_SM3_IV)
    msg = bytes(data)
    bitlen = (len(msg) * 8) & 0xFFFFFFFFFFFFFFFF
    msg += b'\x80'
    msg += b'\x00' * ((56 - len(msg) % 64) % 64)
    msg += bitlen.to_bytes(8, 'big')
    for i in range(0, len(msg), 64):
        v = _sm3_compress(v, msg[i:i + 64])
    return b''.join(x.to_bytes(4, 'big') for x in v)

DEFAULT_URL = "https://cdrm.live.cntv.cn:444/udrm/udmCNTVGetLicense"
DEFAULT_DEVICE_ID = "1002881000000686"
DEFAULT_MAC = ""
DEFAULT_OPERATOR = "18"
DEFAULT_APK = "v1.1.1-6"
DEFAULT_LIB = "v1.1.20251201001m"
SIG_PREFIX = "CNTV"
SIG_SALT = "21A28554279E4A7B"

_TEMPLATE = {
    "version": "3.2-UDRM-CNTV",
    "platform": "5",
    "cdn": "1",
    "udrmTerminalLibVersion": "v1.1.20251201001m",
    "operatorID": "15",
    "apkVersion": "cbox2100",
    "extensions": [
        {
            "critical": "true",
            "identifier": base64.b64encode(DEFAULT_MAC.encode()).decode(),
            "name": "mac"
        }
    ],
    "deviceID": "MTAwMjg4MTAwMDAwMDY4Ng==",
    "drmServerID": "CNTV",
    "nonce": "",
    "timestamp": "1531368146",
    "contentIDs": "",
    "certificateChain": [
        "MTAwMjg4MTAwMDAwMDYyOQ=="
    ],
    "signature": ""
}


def _b64(data: bytes) -> str:
    return base64.b64encode(data).decode()


def build_request(kid: str, nonce: str | None = None,
                  device_id: str = DEFAULT_DEVICE_ID,
                  mac: str = DEFAULT_MAC,
                  operator: str = DEFAULT_OPERATOR,
                  apk_version: str = DEFAULT_APK,
                  lib_version: str = DEFAULT_LIB) -> tuple[str, str]:
    if nonce is None:
        nonce = _b64(os.urandom(32))
    body = _TEMPLATE.copy()
    body["nonce"] = nonce
    body["contentIDs"] = [_b64(kid.encode())]
    body = json.dumps(body, separators=(",", ":"))
    sig = _b64(sm3((SIG_PREFIX + body + SIG_SALT).encode()))
    return body.replace('"signature":""', '"signature":"%s"' % sig), nonce


def post(url: str, body: str, timeout: float = 20.0,
         verify_tls: bool = False) -> str:
    ctx = ssl.create_default_context()
    if not verify_tls:
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
    req = urllib.request.Request(
        url, data=body.encode(),
        headers={"Accept": "*/*", "Content-Type": "application/json"},
        method="POST")
    with urllib.request.urlopen(req, context=ctx, timeout=timeout) as resp:
        return resp.read().decode("utf-8", "replace")


def derive_cek(license_bytes: bytes, kid: str, request_nonce: str,
               device_id: str, response_nonce: str) -> tuple[bytes, bytes]:
    key2 = sm3(("cntv" + kid + request_nonce + device_id + response_nonce)
               .encode())[:16]
    cek = bytes(a ^ b for a, b in zip(license_bytes, key2))
    return cek, key2


def fetch_cek(url: str = DEFAULT_URL, kid: str | None = None,
              device_id: str = DEFAULT_DEVICE_ID, mac: str = DEFAULT_MAC,
              nonce: str | None = None, timeout: float = 20.0,
              verify_tls: bool = False,
              kid_b64: str | None = None) -> dict:
    if kid is None:
        if kid_b64 is None:
            raise ValueError("kid or kid_b64 required")
        kid = base64.b64decode(kid_b64).decode()
    body, nonce = build_request(kid, nonce, device_id, mac)
    raw = post(url, body, timeout, verify_tls)
    try:
        resp = json.loads(raw)
    except ValueError:
        raise RuntimeError("server did not return JSON: %r" % raw[:200])

    if resp.get("status") != 0:
        raise RuntimeError("licence server error: status=%s message=%s" %
                           (resp.get("status"), resp.get("message")))

    entries = []
    for pl in resp.get("protectedLicenses", []):
        cid = base64.b64decode(pl.get("contentId", "")).decode("ascii", "replace")
        for li in pl.get("licenses", []):
            rkid = base64.b64decode(li.get("licenseID", "")).decode("ascii", "replace")
            lic = base64.b64decode(li.get("license", ""))
            cek, key2 = derive_cek(lic, rkid, nonce, device_id,
                                   resp.get("nonce", ""))
            entries.append({"cid": cid, "kid": rkid, "license": lic,
                            "key2": key2, "cek": cek})
    if not entries:
        raise RuntimeError("no licence in response: %r" % raw[:200])

    out = entries[0]
    out.update({"req_nonce": nonce, "resp_nonce": resp.get("nonce", ""),
                "raw": resp})
    kid = out["kid"]
    cek = out["cek"]
    return out
if __name__ == '__main__':
    kid ="o0gfroc3000000000000000000000012"
    out=fetch_cek(kid=kid)
    print(out)