#!/bin/sh
# pgpenc against gpg: usage gpgtest.sh ./build/pgpenc
P=${1:-./build/pgpenc}
T=${TMPDIR:-/tmp}/pgpenc.$$
fails=0
ok() { echo "  ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails+1)); }
if ! command -v gpg >/dev/null 2>&1; then echo "pgpenc: gpg not installed, skipped"; exit 0; fi
mkdir -p $T && chmod 700 $T
trap 'gpgconf --homedir $T --kill all 2>/dev/null; rm -rf $T' 0
export GNUPGHOME=$T
for bits in 2048 3072; do
	cat > $T/batch$bits <<EOK
%no-protection
Key-Type: RSA
Key-Length: $bits
Key-Usage: sign
Subkey-Type: RSA
Subkey-Length: $bits
Subkey-Usage: encrypt
Name-Real: Galtzagorri $bits
Name-Email: g$bits@example.org
Expire-Date: 0
%commit
EOK
	gpg --batch --quiet --gen-key $T/batch$bits 2>/dev/null || { bad "gpg --gen-key $bits"; continue; }
	gpg --export g$bits@example.org > $T/pub$bits.gpg
	gpg --armor --export g$bits@example.org > $T/pub$bits.asc
	for k in pub$bits.gpg pub$bits.asc; do
		$P -k $T/$k -i > $T/info.txt || bad "-i $k"
		id=$(gpg --list-keys --with-colons g$bits@example.org | awk -F: '$1=="sub"{print $5}' | tail -1)
		grep -q "key id $id" $T/info.txt && ok "$bits $k: the key id is the encryption subkey's ($id)" || bad "$bits $k: key id ($id vs $(head -1 $T/info.txt))"
		for size in 0 1 15 16 17 100 511 512 513 1023 1024 5000 70000; do
			head -c $size /dev/urandom > $T/in.bin
			for a in "" "-a"; do
				rm -f $T/out.enc $T/back.bin
				$P $a -k $T/$k -o $T/out.enc $T/in.bin || { bad "$bits $k size $size $a: pgpenc"; continue; }
				gpg --batch --quiet --decrypt $T/out.enc > $T/back.bin 2>$T/gpg.err
				if cmp -s $T/in.bin $T/back.bin; then ok "$bits $k size $size $a: gpg -d gives the file back"
				else bad "$bits $k size $size $a: gpg -d differs"; cat $T/gpg.err; fi
			done
		done
	done
done
# no output file left behind on a bad key
echo "not a key" > $T/junk
$P -k $T/junk -i 2>/dev/null; [ $? -eq 1 ] && ok "a file that is not a key: exit 1" || bad "junk key"
$P 2>/dev/null; [ $? -eq 2 ] && ok "usage: exit 2" || bad "usage"
[ $fails -eq 0 ] && echo "pgpenc: PASS" || { echo "pgpenc: $fails FAIL"; exit 1; }
