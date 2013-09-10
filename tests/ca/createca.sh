#!/bin/sh

set -e

# gen keys
echo Generate ca.key
openssl genrsa -out ca.key 4096
echo Generate intermediate.key
openssl genrsa -out intermediate.key 2048
echo Generate server.key
openssl genrsa -out server.key 2048

echo Generate self-signed ca.crt
openssl req -new -x509 -out ca.crt -key ca.key -config ca.conf -days 36500 -set_serial 01

echo Generate intermediate.csr
openssl req -new -out intermediate.csr -key intermediate.key -config intermediate.conf -days 36500
echo Generate intermediate.crt
openssl x509 -req \
	-in intermediate.csr \
	-out intermediate.crt \
	-extfile intermediate.conf -extensions v3_ca \
	-CA ca.crt -CAkey ca.key -CAserial ca.srl \
	-days 36500 
rm intermediate.csr

echo "0000000000000001" > ca.srl
echo "0000000000000001" > intermediate.srl

for name in test1.ssl test2.ssl; do
	echo Generate server_${name}.csr
	openssl req -new -out server_${name}.csr -key server.key -config server_${name}.conf -days 36500
	echo Generate server_${name}.crt
	openssl x509 -req \
		-in server_${name}.csr \
		-out server_${name}.crt \
		-extfile server_${name}.conf -extensions v3 \
		-CA intermediate.crt -CAkey intermediate.key -CAserial intermediate.srl \
		-days 36500
	rm server_${name}.csr

	echo Generate server_${name}.pem
	cat server.key server_${name}.crt intermediate.crt > server_${name}.pem
done
