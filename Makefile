# ------------------------------------------------------------------
#   Makefile
#   Copyright (C) 2019-2020 Divon Lan <divon@genozip.com>
#   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

# Note for Windows: to run this make, you need mingw (for the gcc compiler) and cygwin (for Unix-like tools):
# Mingw: http://mingw-w64.org/doku.php  (Windows 32 bit version also works)
# Cygwin: https://www.cygwin.com/

ifdef BUILD_PREFIX
IS_CONDA=1
endif

LDFLAGS     += -lpthread -lm

ifdef IS_CONDA 
	CFLAGS  += -Wall -I. -D_LARGEFILE64_SOURCE=1 
	LDFLAGS += -lbz2 # conda - dynamic linking with bz2 

	ifeq ($(OS),Windows_NT)
		CC=gcc # in Windows, override conda's default Visual C with gcc 
		LDFLAGS += -L$(PREFIX)/Library/lib
		CFLAGS  += -I$(PREFIX)/Library/include 
	endif
else
	CC=gcc
	CFLAGS = -Wall -I. -Izlib -Ibzlib -D_LARGEFILE64_SOURCE=1
endif 


MY_SRCS = genozip.c base250.c move_to_front.c strings.c stats.c arch.c license.c data_types.c \
          zip.c piz.c seg.c zfile.c   \
		  vcf_zip.c vcf_piz.c vcf_seg.c vcf_zfile.c vcf_gloptimize.c vcf_vblock.c vcf_gtshark.c vcf_squeeze.c vcf_samples.c vcf_header.c \
          sam_zip.c sam_piz.c sam_shared.c \
		  fasta.c fastq.c fast_shared.c \
		  gff3.c me23.c \
		  buffer.c random_access.c sections.c compressor.c base64.c \
	      txtfile.c profiler.c file.c dispatcher.c crypt.c aes.c md5.c \
		  vblock.c regions.c  optimize.c dict_id.c hash.c stream.c url.c

CONDA_COMPATIBILITY_SRCS = compatibility/visual_c_pthread.c compatibility/visual_c_gettime.c compatibility/visual_c_misc_funcs.c compatibility/mac_gettime.c

ZLIB_SRCS  = zlib/gzlib.c zlib/gzread.c zlib/inflate.c zlib/inffast.c zlib/zutil.c zlib/inftrees.c zlib/crc32.c zlib/adler32.c   

BZLIB_SRCS = bzlib/blocksort.c bzlib/bzlib.c bzlib/compress.c bzlib/crctable.c bzlib/decompress.c bzlib/huffman.c bzlib/randtable.c

LZMA_SRCS  = lzma/LzmaEnc.c lzma/LzmaDec.c lzma/LzFind.c

CONDA_DEVS = Makefile .gitignore test-file.vcf 

CONDA_DOCS = LICENSE.non-commercial.txt LICENSE.commercial.txt AUTHORS README.md

CONDA_INCS = aes.h dispatcher.h optimize.h profiler.h dict_id.h txtfile.h zip.h vcf_v1.c \
             base250.h endianness.h md5.h sections.h section_types.h text_help.h strings.h hash.h stream.h url.h \
             buffer.h file.h move_to_front.h seg.h text_license.h version.h compressor.h stats.h \
             crypt.h genozip.h piz.h vblock.h zfile.h random_access.h regions.h \
			 arch.h license.h data_types.h base64.h \
			 vcf.h vcf_private.h sam.h sam_private.h me23.h fasta.h fastq.h fast_private.h gff3.h \
             compatibility/visual_c_getopt.h compatibility/visual_c_unistd.h \
             compatibility/visual_c_gettime.h compatibility/visual_c_stdint.h compatibility/visual_c_misc_funcs.h \
             compatibility/visual_c_pthread.h \
             compatibility/mac_gettime.h  \
			 zlib/crc32.h zlib/gzguts.h zlib/inffast.h zlib/inffixed.h zlib/inflate.h zlib/inftrees.h zlib/zconf.h \
			 zlib/zlib.h zlib/zutil.h \
			 lzma/7zTypes.h lzma/Compiler.h lzma/LzFind.h lzma/LzFindMt.h lzma/LzHash.h lzma/LzmaDec.h lzma/LzmaEnc.h \
			 lzma/Precomp.h lzma/Threads.h

ifeq ($(CC),cl)
	MY_SRCS += compatibility/visual_c_gettime.c compatibility/visual_c_misc_funcs.c compatibility/visual_c_pthread.c
endif

ifeq ($(OS),Windows_NT)
# Windows
	EXE = .exe
	LDFLAGS += -static -static-libgcc
	LZMA_SRCS += lzma/Threads.c lzma/LzFindMt.c
else
	CFLAGS += -D_7ZIP_ST
    uname := $(shell uname -s)
    ifeq ($(uname),Linux)
# Linux
        LDFLAGS += -lrt
    endif
    ifeq ($(uname),Darwin)
# Mac
		MY_SRCS += compatibility/mac_gettime.c
    endif
endif

ifndef IS_CONDA 
# local - static link everything
SRCS = $(MY_SRCS) $(ZLIB_SRCS) $(BZLIB_SRCS) $(LZMA_SRCS)
else 
# conda - use packages for bzip2
SRCS = $(MY_SRCS) $(ZLIB_SRCS) $(LZMA_SRCS)
endif

OBJS       := $(SRCS:.c=.o)
DEBUG_OBJS := $(SRCS:.c=.debug-o)
OPT_OBJS   := $(SRCS:.c=.opt-o) # optimized but with debug info, for debugging issues that only manifest with compiler optimization

DEPS       := $(SRCS:.c=.d)

EXECUTABLES       = genozip$(EXE)       genounzip$(EXE)       genocat$(EXE)       genols$(EXE)
DEBUG_EXECUTABLES = genozip-debug$(EXE) genounzip-debug$(EXE) genocat-debug$(EXE) genols-debug$(EXE)
OPT_EXECUTABLES   = genozip-opt$(EXE)   genounzip-opt$(EXE)   genocat-opt$(EXE)   genols-opt$(EXE)

ifeq ($(CC),gcc)
	OPTFLAGS += -Ofast -std=gnu99
else
	OPTFLAGS += -O2
endif

all   : CFLAGS += $(OPTFLAGS) 
all   : $(EXECUTABLES) LICENSE.non-commercial.txt

debug : CFLAGS += -DDEBUG -g -O0
debug : $(DEBUG_EXECUTABLES) LICENSE.non-commercial.txt

opt   : CFLAGS += -g $(OPTFLAGS)
opt   : $(OPT_EXECUTABLES) LICENSE.non-commercial.txt

-include $(DEPS)

%.d: %.c
	@echo Calculating dependencies $<
	@$(CC) $(CFLAGS) -MM -MT $@ $< -MF $(@:%.o=%.d)

%.o: %.c %.d
	@echo Compiling $<
	@$(CC) -c -o $@ $< $(CFLAGS)

%.debug-o: %.c %.d
	@echo "Compiling $< (debug)"
	@$(CC) -c -o $@ $< $(CFLAGS)

%.opt-o: %.c %.d
	@echo "Compiling $< (opt)"
	@$(CC) -c -o $@ $< $(CFLAGS)

genozip$(EXE): $(OBJS)
	@echo Linking $@
	@$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS)
 
genozip-debug$(EXE): $(DEBUG_OBJS)
	@echo Linking $@
	@$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS)

genozip-opt$(EXE): $(OPT_OBJS)
	@echo Linking $@
	@$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS)

genounzip$(EXE) genocat$(EXE) genols$(EXE): genozip$(EXE)
	@echo Hard linking $@
	@rm -f $@ 
	@ln $^ $@

genounzip-debug$(EXE) genocat-debug$(EXE) genols-debug$(EXE): genozip-debug$(EXE)
	@echo Hard linking $@
	@rm -f $@ 
	@ln $^ $@

genounzip-opt$(EXE) genocat-opt$(EXE) genols-opt$(EXE): genozip-opt$(EXE)
	@echo Hard linking $@
	@rm -f $@ 
	@ln $^ $@

LICENSE.non-commercial.txt: text_license.h
	@echo Generating $@
	@./genozip$(EXE) --license > $@

# this is used by build.sh to install on conda for Linux and Mac. Installation for Windows in in bld.bat
install: genozip$(EXE)
	@echo Installing in $(PREFIX)/bin
	@if ( test ! -d $(PREFIX)/bin ) ; then mkdir -p $(PREFIX)/bin ; fi
	@cp -f genozip$(EXE) $(PREFIX)/bin/genozip$(EXE)
ifneq ($(OS),Windows_NT)
	@chmod a+x $(PREFIX)/bin/genozip$(EXE)
endif
	@cp -f $(PREFIX)/bin/genozip$(EXE) $(PREFIX)/bin/genounzip$(EXE)
	@cp -f $(PREFIX)/bin/genozip$(EXE) $(PREFIX)/bin/genocat$(EXE)
	@cp -f $(PREFIX)/bin/genozip$(EXE) $(PREFIX)/bin/genols$(EXE)

version = $(shell head -n1 version.h |cut -d\" -f2)

SH_VERIFY_ALL_COMMITTED = (( `git status|grep 'modified\|Untracked files'|grep -v .gitkeep |wc -l ` == 0 )) || \
                          (echo ERROR: there are some uncommitted changes: ; echo ; git status ; exit 1)

# currently, I build for conda from my Windows machine so I don't bother supporting other platforms
ifeq ($(OS),Windows_NT)

# increments minor version, eg. 1.0.1 -> 1.0.2. 
# To increment a major version, manually edit version.h and set minor version to -1 e.g. 1.1.-1 (careful! no newlines or spaces)
# and re-compile so that genozip --version gets updated
# IMPORTANT: the first number in the version indicates the genozip file format version and goes into
# the genozip file header SectionHeaderTxtHeader.genozip_version
increment-version: $(MY_SRCS) $(ZLIB_SRCS) $(BZLIB_SRCS) $(LZMA_SRCS) $(CONDA_COMPATIBILITY_SRCS) $(CONDA_DEVS) $(CONDA_DOCS) $(CONDA_INCS) # note: target name is not "version.h" so this is not invoked during "make all" or "make debug"
	@echo "Incrementing version.h"
	@$(SH_VERIFY_ALL_COMMITTED)
	@bash increment-version.sh
	@git commit -m "increment version" version.h 

decrement-version:
	@echo "Do manually:"
	@echo "Remove tag: git push --delete origin genozip-a.b.c"
	@echo "Change version.h to the last version that still has a tag"

.archive.tar.gz : increment-version $(MY_SRCS) $(ZLIB_SRCS) $(BZLIB_SRCS) $(LZMA_SRCS) $(CONDA_COMPATIBILITY_SRCS) $(CONDA_DEVS) $(CONDA_DOCS) $(CONDA_INCS) 
	@echo Creating github tag genozip-$(version) and archive
	@$(SH_VERIFY_ALL_COMMITTED)
	@git push 
	@git tag genozip-$(version)
	@git push origin genozip-$(version)
	@curl https://github.com/divonlan/genozip/archive/genozip-$(version).tar.gz --silent --location -o $@

conda/meta.yaml: conda/meta.template.yaml .archive.tar.gz
	@echo "Generating meta.yaml (for conda)"
	@(  sed /__README_MD__/Q conda/meta.template.yaml ; \
	    cat README.md | sed "s/<.\{1,3\}>//g" | grep -v "<!" | sed 's/^/    /' ; \
		sed '0,/__README_MD__/d' conda/meta.template.yaml \
	 ) | \
		sed s/__SHA256__/$(shell openssl sha256 .archive.tar.gz | cut -d= -f2 | cut -c2-)/ | \
		sed s/__VERSION__/$(version)/g | \
		grep -v "^#" \
		> $@

#CONDA_RECIPE_DIR = ../staged-recipes/recipes/genozip # initial stage-recipes step, keeping here for future reference
CONDA_RECIPE_DIR = ../genozip-feedstock/recipe

# publish to conda-forge 
conda/.conda-timestamp: conda/meta.yaml conda/build.sh conda/bld.bat
	@echo "Publishing to conda-forge"
	@$(SH_VERIFY_ALL_COMMITTED)
	@echo " "
	@echo "Copying meta.yaml build.sh bld.bat to conda-forge"
	@cp conda/meta.yaml conda/build.sh conda/bld.bat $(CONDA_RECIPE_DIR)
	@echo "Committing my files to branch genozip on my fork"
	@(cd $(CONDA_RECIPE_DIR); git commit -m "update" meta.yaml build.sh bld.bat; git push)
	@echo " "
	@echo "Submitting pull request to conda-forge"
#	@(cd $(CONDA_RECIPE_DIR); git request-pull master https://github.com://conda-forge/genozip-feedstock master)
#	@(cd $(CONDA_RECIPE_DIR); git request-pull master https://github.com/divonlan/genozip-feedstock master)
	@touch $@
	@echo "CONDA: Using a browser:"
	@echo "  (1) Go to https://github.com/conda-forge/genozip-feedstock/pulls"
	@echo "  (2) Click 'Compare and pull request' then 'Create pull request' and wait 30 seconds for the test to start"
	@echo "      Fallback: if you can't see 'Compare & pull', manually created a pull request 'into conda-forge:master from divonlan:genozip'"
	@echo "  (3) Go to https://dev.azure.com/conda-forge/feedstock-builds/_build"
	@echo "  (4) Click on genozip and wait (~5 min) for the test to complete. Fix any issues."
	@echo "  (5) Go back to the tab in (2) and click 'Merge pull request' and the 'Confirm merge' (DONT CLICK 'Delete branch')"
	@echo "  (6) Go to https://dev.azure.com/conda-forge/feedstock-builds/_build and watch the build - it should be fine"
	@echo "  (7) In ~30 minutes users will be able to 'conda update genozip'"

windows/%.exe: %.exe
	@echo Copying $<
	@cp -f $< $@ 

windows/readme.txt: $(EXECUTABLES)
	@echo Generating $@
	@./genozip$(EXE)   --help  > $@
	@printf '%.s-' {1..120}   >> $@
	@./genounzip$(EXE) --help >> $@
	@printf '%.s-' {1..120}   >> $@
	@./genols$(EXE)    --help >> $@
	@printf '%.s-' {1..120}   >> $@
	@./genocat$(EXE)   --help >> $@

windows/LICENSE.for-installer.txt: text_license.h
	@echo Generating $@
	@./genozip$(EXE) --license --force > $@

WINDOWS_INSTALLER_OBJS = windows/genozip.exe windows/genounzip.exe windows/genocat.exe windows/genols.exe \
                         windows/LICENSE.for-installer.txt windows/readme.txt

# this must be run ONLY has part of "make distribution" or else versions will be out of sync
windows/genozip-installer.exe: $(WINDOWS_INSTALLER_OBJS) LICENSE.commercial.txt LICENSE.non-commercial.txt
	@echo 'Creating Windows installer'
	@$(SH_VERIFY_ALL_COMMITTED)
	@echo 'WINDOWS: Using the UI:'
	@echo '  (1) Open windows/genozip.ifp'
	@echo '  (2) Set General-Program version to $(version)'
	@echo '  (3) Verify the files Setup-Files, and the license from LICENSE.for-installer.txt are up to date'
	@echo '  (4) Click Save, then click Build, then click No to the popup question'
	@echo '  (5) Exit the UI (close the window)'
	@(C:\\\\Program\\ Files\\ \\(x86\\)\\\\solicus\\\\InstallForge\\\\InstallForge.exe ; exit 0)
	@echo 'Committing Windows installer and pushing to repo'
	@(git stage windows/genozip.ifp $@ ; exit 0)
	@(git commit -m windows_files_for_version_$(version) windows/genozip.ifp $@ ; exit 0)
	@git push

mac/.remote_mac_timestamp: # to be run from Windows to build on a remote mac
	@echo "Creating Mac installer"
	@$(SH_VERIFY_ALL_COMMITTED)
	@echo "Pushing all committed changes to github"
	@(( `git push |& grep "Everything up-to-date"  | wc -l` > 0 )) || (echo "Pushed some stuff... waiting 5 seconds for it to settle in" ; sleep 5)
	@echo "Logging in to remote mac" 
	@# Get IP address - check if the previous IP address still works or ask for a new one. Assuming a LAN on an Android hotspot.
	@ip=`cat mac/.mac_ip_address` ; a=`echo $$ip|cut -d. -f4`; (( `ping  -n 1 $$ip | grep "round trip times" | wc -l` > 0 )) || read -p "IP Address: 192.168.43." a ; ip=192.168.43.$$a ; echo $$ip > mac/.mac_ip_address
	@[ -f mac/.mac_username ] || ( echo ERROR: file mac/.mac_username missing && exit 1 )
	@ssh `cat mac/.mac_ip_address` -l `cat mac/.mac_username`  "cd genozip ; echo "Pulling from git" ; git pull >& /dev/null ; make mac/.from_remote_timestamp" # pull before make as Makefile might have to be pulled
	@touch $@

distribution: conda/.conda-timestamp windows/genozip-installer.exe mac/.remote_mac_timestamp
	
endif # Windows

ifeq ($(uname),Darwin)

MACDWNDIR = mac/darwinpkg
MACLIBDIR = $(MACDWNDIR)/Library/genozip
MACSCTDIR = mac/scripts
MACRSSDIR = mac/Resources

$(MACRSSDIR)/welcome.html: mac/welcome.template.html
	@sed -e "s/__VERSION__/$(version)/g" $< > $@ 

$(MACSCTDIR)/postinstall: mac/postinstall.template.sh
	@sed -e "s/__FILES__/$(EXECUTABLES)/g" $< > $@

$(MACLIBDIR)/uninstall.sh: mac/uninstall.template.sh
	@sed -e "s/__VERSION__/$(version)/g" $< | sed -e "s/__FILES__/$(EXECUTABLES)/g" > $@ 

$(MACRSSDIR)/README.html: README.md
	@cp -f $< $@

$(MACRSSDIR)/LICENSE.non-commercial.txt: LICENSE.non-commercial.txt
	@cp -f $< $@

$(MACLIBDIR)/%: %
	@cp -f $< $@

pkg_identifier  := genozip-$(version)

app_specific_pw := $(shell cat .altool_app_specifc_password)

apple_id        := $(shell /usr/libexec/PlistBuddy -c "print :Accounts:0:AccountID" ~/Library/Preferences/MobileMeAccounts.plist)

signer_name     := ${shell security find-identity -v|grep "3rd Party Mac Developer Installer" | cut -d ":" -f2 | cut -d"(" -f1 }

mac/genozip.pkg: $(MACLIBDIR)/genozip $(MACLIBDIR)/genounzip $(MACLIBDIR)/genocat $(MACLIBDIR)/genols $(MACLIBDIR)/uninstall.sh \
                 $(MACSCTDIR)/postinstall
	@echo "Building Mac package $@"
	@$(SH_VERIFY_ALL_COMMITTED)
	@chmod -R 755 $(MACLIBDIR) $(MACSCTDIR)				 
	@pkgbuild --identifier $(pkg_identifier) --version $(version) --scripts $(MACSCTDIR) --root $(MACDWNDIR) mac/genozip.pkg > /dev/null

mac/genozip_installer.unsigned.pkg: mac/genozip.pkg mac/Distribution \
                                    $(MACRSSDIR)/welcome.html $(MACRSSDIR)/README.html $(MACRSSDIR)/LICENSE.non-commercial.txt
	@echo "Building Mac product $@"
	@productbuild --distribution mac/Distribution --resources $(MACRSSDIR) --package-path mac $@ > /dev/null

mac/genozip_installer.pkg: mac/genozip_installer.unsigned.pkg
	@bash -c "echo -n Unlocking the keychain. Password:"
	@# note: keychain requires unlocking if logged in remotely (through SSH)
	@read pw ; security -v unlock-keychain -p $$pw `security list-keychains|grep login|cut -d\" -f2`
	@echo "Signing Mac product $@"
	@# note: productsign needs a "3rd party mac developer" certificate, and the Apple developer CA certificate, installed in the keychain. see: https://developer.apple.com/developer-id/. I keep them on Drive for backup.
	@productsign --sign "$(signer_name)" $< $@ > /dev/null
	@echo "Verifying the signature"
	@(( `pkgutil --check-signature $@ | grep "signed by a developer certificate issued by Apple (Development)" | wc -l ` > 0 )) || (echo Error: signature verification failed ; exit 1)
	@#@echo 'Committing Mac installer and pushing to repo'
	@#@(git stage $@ ; exit 0)
	@#(git commit -m mac_installer_for_version_$(version) $@ ; exit 0)
	@#git push

mac/.from_remote_timestamp: mac/genozip_installer.pkg
	@echo "Notarizing Mac app"
	@xcrun altool --notarize-app --primary-bundle-id $(pkg_identifier) --username $(apple_id) --password $(app_specific_pw) --file $< >& .notarize.out ; exit 0
	@grep ERROR\: .notarize.out ; exit 0
	@(( `grep ERROR\: .notarize.out | wc -l` == 0 )) || (echo "See .notarize.out" ; exit 1)
	@(( `grep "No errors uploading" .notarize.out | wc -l` == 0 )) || (grep "RequestUUID" .notarize.out ; exit 0)
	@touch $@

endif # Darwin

test:
	@cat test.sh | tr -d "\r" | bash -

clean-debug:
	@echo Cleaning up debug
	@rm -f $(DEBUG_OBJS) $(DEBUG_EXECUTABLES) *.debug-o
	@rm -f $(OPT_OBJS) $(OPT_EXECUTABLES) *.opt-o

clean-optimized:
	@echo Cleaning up optimized
	@rm -f $(OBJS) $(EXECUTABLES) *.o

clean: clean-debug clean-optimized
	@echo Cleaning up
	@rm -f $(DEPS) $(WINDOWS_INSTALLER_OBJS) *.d

.PHONY: clean clean-debug clean-optimized git-pull macos mac/.remote_mac_timestamp

