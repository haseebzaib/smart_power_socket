#!/usr/bin/env python3
"""Export only the configured MCUboot Ed25519 public key into an app header."""
import argparse
from pathlib import Path
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
p=argparse.ArgumentParser()
p.add_argument('--key',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
b=a.key.read_bytes()
try: key=serialization.load_pem_private_key(b,password=None).public_key()
except ValueError: key=serialization.load_pem_public_key(b)
if not isinstance(key,Ed25519PublicKey): raise SystemExit('SPS OTA requires the MCUboot Ed25519 key')
der=key.public_bytes(serialization.Encoding.DER,serialization.PublicFormat.SubjectPublicKeyInfo)
raw=key.public_bytes(serialization.Encoding.Raw,serialization.PublicFormat.Raw)
text='#pragma once\n#include <array>\n#include <cstdint>\nnamespace ota_public_key {\n'
for name,value in [('der',der),('raw',raw)]:
 text+=f'inline constexpr std::array<uint8_t, {len(value)}> {name} = {{'+','.join(f'0x{x:02x}' for x in value)+'};\n'
text+='}\n'
a.output.parent.mkdir(parents=True,exist_ok=True)
a.output.write_text(text)
