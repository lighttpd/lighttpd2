#!/bin/bash

set -e

# (requires gnutls >= 3.2.7 (or >= 3.1.17 and < 3.2.0))

KEY_TYPE="${KEY_TYPE:-rsa}"
HASH_ALG="${HASH_ALG:-SHA512}"

function gen_rsa_key() {
	local name="$1"
	local security="${2:-high}"
	local secparam=(--sec-param "${security}")

	echo "Generate RSA key into ${name}.key and ${name}.pub"
	certtool -p --rsa --outfile "${name}.key" "${secparam[@]}"
	certtool --load-privkey "${name}.key" --pubkey-info --outfile "${name}.pub"
}

function gen_ecc_key() {
	local name="$1"
	local security="${2:-ultra}"
	local secparam=(--sec-param "${security}")

	echo "Generate ECC key into ${name}.key and ${name}.pub"
	certtool -p --ecc --outfile "${name}.key" "${secparam[@]}"
	certtool --load-privkey "${name}.key" --pubkey-info --outfile "${name}.pub"
}

function gen_key() {
	case "${KEY_TYPE}" in
	rsa) gen_rsa_key "$@" ;;
	ecc) gen_ecc_key "$@" ;;
	*) echo >&2 "Unknown key type: ${KEY_TYPE}"; exit 1 ;;
	esac
}

function ca_sign_self() {
	local ca_name="$1"

	echo "Self signing ${ca_name}"
	certtool -s "--hash=${HASH_ALG}" --load-privkey "${ca_name}.key" --outfile "${ca_name}.crt" --template "${ca_name}.template"
}

function ca_sign() {
	local ca_name="$1"
	local subject_name="$2"
	local key_name="${3:-${subject_name}}"

	echo "Signing ${subject_name} (key ${key_name}) with ${ca_name}"
	certtool -c "--hash=${HASH_ALG}" --load-ca-certificate "${ca_name}.crt" --load-ca-privkey "${ca_name}.key" --load-pubkey "${key_name}.pub" --outfile "${subject_name}.crt" --template "${subject_name}.template"
}

# gen keys
gen_key "ca"
gen_key "intermediate"
gen_key "server"

ca_sign_self "ca"
ca_sign "ca" "intermediate"

for name in test1.ssl test2.ssl; do
	ca_sign "intermediate" "server_${name}" "server"

	echo "Generate server_${name}.pem"
	cat "server.key" "server_${name}.crt" "intermediate.crt" > "server_${name}.pem"
done
