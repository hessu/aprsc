#!/bin/bash

set -e

export PATH=$PATH:/opt/src/c/cov-analysis-linux64-6.5.1/bin

make clean
cov-build --dir cov-int make
tar cvfz aprsc.tgz cov-int
rm -rf cov-int

VERSION=`cat VERSION`
PASSWORD=`cat ~/.covpw`

echo "Uploading version $VERSION to Coverity..."

curl --form file=@aprsc.tgz --form project=aprsc \
	--form password="$PASSWORD" \
	--form email=hessu@hes.iki.fi \
	--form version="$VERSION" \
	--form description="" \
	http://scan5.coverity.com/cgi-bin/upload.py

rm -f aprsc.tgz

