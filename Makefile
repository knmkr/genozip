# ------------------------------------------------------------------
#   Makefile
#   Copyright (C) 2019-2020 Divon Lan <genozip@blackpawventures.com>
#   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

# Note for Windows: to run this make, you need mingw (for the gcc compiler) and cygwin (for Unix-like tools):
# Mingw: http://mingw-w64.org/doku.php  (Windows 32 bit version also works)
# Cygwin: https://www.cygwin.com/

VERSION = 1.0.0

EXE =
ifeq ($(OS),Windows_NT)
	EXE = .exe
endif

CC=gcc
CFLAGS       = -Ibzlib -Izlib -D_LARGEFILE64_SOURCE=1 -Wall -Ofast 
CFLAGS_DEBUG = -Ibzlib -Izlib -D_LARGEFILE64_SOURCE=1 -Wall -DDEBUG -g
LIBS = -lpthread -lm

DEVS = Makefile .gitignore genozip.code-workspace \
       .vscode/c_cpp_properties.json .vscode/launch.json .vscode/settings.json .vscode/tasks.json \
       test-file.vcf \
       conda/build.sh.template conda/bld.bat.template conda/meta.yaml.template

DOCS = LICENSE.non-commercial.txt LICENSE.commercial.txt AUTHORS README.md \
       bzlib/LICENSE.bzlib bzlib/README.bzlib \
	   zlib/README.zlib

INCS = genozip.h lic-text.h \
       bzlib/bzlib.h bzlib/bzlib_private.h \
	   zlib/crc32.h zlib/gzguts.h zlib/inffast.h zlib/inffixed.h zlib/inflate.h zlib/inftrees.h zlib/zconf.h zlib/zlib.h zlib/zutil.h \
	   mac/mach_gettime.h

SRCS = base250.c move_to_front.c vcf_header.c zip.c piz.c gloptimize.c buffer.c main.c \
	   vcffile.c squeeze.c zfile.c segregate.c profiler.c file.c vb.c dispatcher.c \
       bzlib/blocksort.c bzlib/bzlib.c bzlib/compress.c bzlib/crctable.c bzlib/decompress.c bzlib/huffman.c bzlib/randtable.c \
       zlib/gzlib.c zlib/gzread.c zlib/inflate.c zlib/inffast.c zlib/zutil.c zlib/inftrees.c zlib/crc32.c zlib/adler32.c \
       mac/mach_gettime.c

OBJS       := $(SRCS:.c=.o)
DEBUG_OBJS := $(SRCS:.c=.debug-o)

DEPS       := $(SRCS:.c=.d)

all: genozip$(EXE) genounzip$(EXE) genocat$(EXE)

debug: genozip-debug$(EXE)

-include $(DEPS)

%.d: %.c
	@echo Calculating dependencies $<
	@$(CC) $(CFLAGS) -MM -MT $@ $< -MF $(@:%.o=%.d)

%.o: %.c %.d
	@echo Compiling $<
	@$(CC) -c -o $@ $< $(CFLAGS)

%.debug-o: %.c %.d
	@echo "Compiling $< (debug)"
	@$(CC) -c -o $@ $< $(CFLAGS_DEBUG)

all: genozip$(EXE) genounzip$(EXE) genocat$(EXE)

genozip$(EXE): $(OBJS)
	@echo Linking $@
	@$(CC) -o $@ $^ $(CFLAGS) $(LIBS)

genozip-debug$(EXE): $(DEBUG_OBJS)
	@echo Linking $@
	@$(CC) -o $@ $^ $(CFLAGS) $(LIBS)

genounzip$(EXE) genocat$(EXE): genozip$(EXE)
	@echo Hard linking $@
	@rm -f $@ 
	@ln $^ $@

TARBALL := archive/genozip-$(VERSION).tar.gz

$(TARBALL): $(SRCS) $(INCS) $(DOCS) $(DEVS)
	@echo "Archiving to $@ - WARNING: Make sure you have no un-pushed changes locally (Makefile doesn't verify this)"
	@tar --create --gzip --file $(TARBALL) $^
	@echo "Committing $(TARBALL) & pushing changes to genozip/master"
	@(git commit -m "update archive" $(TARBALL) ; git push)

# currently, I build for conda from my Windows machine so I don't bother supporting other platforms
ifeq ($(OS),Windows_NT)

# to publish to conda: 
# 1. Make conda - this will also copy meta.yaml to the local staged-recipes which should be checked out
#    to the branch genozip-branch
# 2. git commit and and git push the file in staged-recipes
# 3. from the github website, go to my forked branch and submit pull request from my branch of my fork to the source master
meta.yaml: conda/meta.yaml.template $(TARBALL)
	@echo "Generating meta.yaml (for conda)"
	@cat conda/meta.yaml.template | \
		sed s/%SHA256/$$(openssl sha256 $(TARBALL)|cut -d= -f2|cut -c2-)/ | \
		sed s/%VERSION/$(VERSION)/g | \
		grep -v "^#" \
		> $@

# we make the build scripts dependents on the archives, so if file list changes, we need to re-generate build
UNIX_SRCS := $(shell echo $(SRCS) | sed 's/\\//\\\\\\//g' ) # a list of files that look like: zlib\/inflate.c
build.sh: conda/build.sh.template 
	@echo "Generating $@ (for conda)"
	@sed 's/%BUILD/\\$$CC $(CFLAGS) $(LIBS) $(UNIX_SRCS) -o genozip/' conda/$@.template > $@
	
WIN_SRCS  := $(shell echo $(SRCS) | sed 's/\\//\\\\\\\\\\\\\\\\/g' ) # crazy! we need 16 blackslashes to end up with a single one in the bld.bat file
bld.bat: conda/bld.bat.template
	@echo "Generating $@ (for conda)"
	@sed 's/%BUILD/%GCC% $(CFLAGS) $(LIBS) $(WIN_SRCS) -o genozip.exe/' conda/$@.template > $@

conda: $(TARBALL) meta.yaml build.sh bld.bat
	@echo "Copying meta.yaml build.sh bld.bat to staged-recipes"
	@cp meta.yaml build.sh bld.bat ../staged-recipes/recipes/genozip/
	@echo "Committing files & pushing changes to my forked staged-recipies/genozip-branch"
	@(cd ../staged-recipes/recipes/genozip; git commit -m "update" meta.yaml build.sh bld.bat; git push)
	@echo " "
	@echo "Now, (1) go to https://github.com/divonlan/staged-recipes/tree/genozip-branch"
	@echo "     (2) select the branch 'genozip-branch'"
	@echo "     (3) click 'New Pull Request'"

endif

LICENSE.non-commercial.txt: genozip$(EXE)
	@echo Generating $@
	@./genozip$(EXE) -L > $@

.PHONY: clean clean-debug clean-all build.sh bld.bat meta.yaml

clean:
	@echo Cleaning up
	@rm -f $(DEPS) $(OBJS) genozip$(EXE) genounzip$(EXE) genocat$(EXE) 

clean-debug:
	@echo Cleaning up debug
	@rm -f $(DEPS) $(DEBUG_OBJS) genozip-debug$(EXE) 
