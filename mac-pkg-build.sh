#!/bin/bash --norc

# ------------------------------------------------------------------
#   mac-pkg-build.sh
#   Copyright (C) 2020 Divon Lan <divon@genozip.com> where applies
#   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

# loosely based on https://github.com/KosalaHerath/macos-installer-builder which is licensed under Apache 2.0 license

MAC_DIR=mac-pkg
TARGET_DIR=${MAC_DIR}/target
VERSION=`head -n1 version.h |cut -d\" -f2`
FILES=(genozip genounzip genols genocat) # array
FILES_STR=${FILES[@]} # string

# copy files to target directory
rm -rf $TARGET_DIR || exit 1
mkdir -p ${TARGET_DIR}/darwinpkg/Library/genozip ${TARGET_DIR}/Resources ${TARGET_DIR}/scripts || exit 1

cp ${FILES[@]} ${TARGET_DIR}/darwinpkg/Library/genozip || exit 1
cp ${MAC_DIR}/Distribution ${TARGET_DIR} || exit 1
cp ${MAC_DIR}/banner.png ${MAC_DIR}/uninstall.sh ${TARGET_DIR}/Resources || exit 1
sed -e "s/__VERSION__/${VERSION}/g" ${MAC_DIR}/welcome.html > ${TARGET_DIR}/Resources/welcome.html || exit 1
cp README.md ${TARGET_DIR}/Resources/README.html || exit 1
cp LICENSE.non-commercial.txt ${TARGET_DIR}/Resources/ || exit 1
sed -e "s/__FILES__/${FILES_STR}/g" ${MAC_DIR}/postinstall > ${TARGET_DIR}/scripts/postinstall || exit 1
sed -e "s/__VERSION__/${VERSION}/g" ${MAC_DIR}/uninstall.sh | sed -e "s/__FILES__/${FILES_STR}/g" > ${TARGET_DIR}/darwinpkg/Library/genozip/uninstall.sh || exit 1

chmod -R 755 ${TARGET_DIR} || exit 1

# build package
pkgbuild --identifier org.genozip.${VERSION} --version ${VERSION} --scripts ${TARGET_DIR}/scripts --root ${TARGET_DIR}/darwinpkg ${MAC_DIR}/genozip.pkg || exit 1

PRODUCT=${MAC_DIR}/genozip-installer.pkg
productbuild --distribution ${TARGET_DIR}/Distribution --resources ${TARGET_DIR}/Resources --package-path ${MAC_DIR} ${PRODUCT} || exit 1

# sign product - IF we have a certificate from Apple
if [ -f apple_developer_certificate_id ]; then
    APPLE_DEVELOPER_CERTIFICATE_ID=`cat apple_developer_certificate_id`
    productsign --sign "Developer ID Installer: ${APPLE_DEVELOPER_CERTIFICATE_ID}" ${PRODUCT} ${PRODUCT}.signed || exit 1
    pkgutil --check-signature ${PRODUCT}.signed || exit 1
    mv -f ${PRODUCT}.signed ${PRODUCT} || exit 1
fi

echo Built mac installer

exit 0