#!/bin/bash

set -e

export PATH=$PATH:/opt/src/c/cov-analysis-linux64-8.5.0.2/bin

make clean
nice cov-build --dir cov-int make -j4
tar cvfz aprsc.tgz cov-int
rm -rf cov-int

VERSION=`cat VERSION`
PASSWORD=`cat ~/.covpw`

echo "Uploading version $VERSION to Coverity..."

curl --form token="$PASSWORD" \
	--form email=hessu@hes.iki.fi \
	--form file=@aprsc.tgz \
	--form version="$VERSION" \
        --form description="" \
        https://scan.coverity.com/builds?project=aprsc

rm -f aprsc.tgz

