#!/binsh

# create new key and cert request
openssl req -new -out ssl.req -subj "/callSign=TESTING/CN=Testing" -newkey rsa:4096 -days 3650 -nodes -config openssl.conf 

# self-sign
openssl x509 -req -in ssl.req -out ssl.crt -signkey privkey.pem

