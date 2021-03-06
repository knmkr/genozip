#!/bin/bash

output=test-output

is_windows=`uname|grep -i mingw`
is_mac=`uname|grep -i Darwin`

# -----------------
# platform settings
# -----------------
if [ -n "$is_windows" ]; then
    path=`pwd| cut -c3-|tr / '\\\\'`\\
else
    path=$PWD/
fi

if `command -v md5 >& /dev/null`; then
    md5=md5 # mac
else
    md5=md5sum 
fi

cmp_2_files() {
    if [[ `$md5 $1 | cut -d" " -f1` != `$md5 ${2%.*} | cut -d" " -f1` ]]; then
        echo "MD5 comparison FAILED!"
        exit 1
    fi
}

test_header() {
    sep="=======================================================================================================\n"
    printf "\n${sep}TESTING $1 \n${sep}"
}

# minimal files - expose edge cases where fields have only 1 instance
files=(minimal.vcf minimal.sam minimal.fq minimal.fa minimal.gvf genome_23andme_Full_minimal.txt)
for file in ${files[@]}; do
    test_header "$file - minimal file test"
    cat $file | tr -d "\r" > unix-nl.$file
    ./genozip unix-nl.$file -ft -o ${output}.genozip || exit 1
done

files=(test-file.vcf test-file.sam test-file.fq test-file.fa test-file.gvf genome_23andme_Full_test-file.txt)
for file in ${files[@]}; do
    test_header "$file - basic test - Unix-style end-of-line"
    cat $file | tr -d "\r" > unix-nl.$file
    ./genozip unix-nl.$file -ft -o ${output}.genozip || exit 1

    test_header "$file - Window-style end-of-line"

    if [ -n "$is_mac" ]; then
        sed 's/$/\13/g' unix-nl.$file > windows-nl.$file || exit 1 # note: sed on mac doesn't recognize \r
    else
        sed 's/$/\r/g' unix-nl.$file > windows-nl.$file || exit 1 # note: sed on mac doesn't recognize \r
    fi

    ./genozip windows-nl.$file -ft -o ${output}.genozip || exit 1
    rm unix-nl.$file windows-nl.$file

    test_header "$file - as URL"
    ./genozip file://${path}$file -ft -o ${output}.genozip || exit 1

    test_header "$file - encrypted"
    ./genozip $file --password abc -ft -o ${output}.genozip || exit 1

    test_header "$file - redirected from stdin"
    cat $file | ./genozip --test --force --output ${output}.genozip --input-type ${file#*.} - || exit 1

    if [ $file != test-file.sam ] && [ $file != genome_23andme_Full_test-file.txt ]; then
        allow_compressed=1;
    else 
        allow_compressed=0;
    fi

    if `command -v gzip >& /dev/null` && [ $allow_compressed == 1 ]; then
        test_header "${file} - with gzip"
        cp $file copy.$file
        gzip copy.$file
        ./genozip copy.${file}.gz -ft -o ${output}.genozip || exit 1
        rm copy.${file}.gz
    fi
    
    if `command -v bzip2 >& /dev/null` && [ $allow_compressed == 1 ]; then
        test_header "${file} - with bzip2"
        cp $file copy.$file
        bzip2 copy.$file
        ./genozip copy.${file}.bz2 -ft -o ${output}.genozip || exit 1
        rm copy.${file}.bz2
    fi
    
    if `command -v xz >& /dev/null` && [ $allow_compressed == 1 ]; then
        test_header "${file} - with xz"
        cp $file copy.$file
        xz copy.$file
        ./genozip copy.${file}.xz -ft -o ${output}.genozip || exit 1
        rm copy.${file}.xz
    fi
        
    if [ -z "$is_windows" ]; then # windows can't redirect binary data
        test_header "$file - redirecting stdout"
        ./genozip ${file} --stdout > ${output}.genozip || exit 1
        ./genounzip ${output}.genozip -f || exit 1
        cmp_2_files $file $output
    fi

    test_header "$file - concat & split"
    file1=copy1.$file
    file2=copy2.$file
    cp $file $file1
    cp $file $file2
    ./genozip $file1 $file2 -ft -o ${output}.genozip || exit 1
    ./genounzip ${output}.genozip -O -t || exit 1
    ls  cop*
    rm $file1 $file2

    test_header "$file --optimize - NOT checking correctness, just that it doesn't crash"
    ./genozip $file -f --optimize -o ${output}.genozip || exit 1

done

file=test-file.vcf
test_header "$file - testing VCF with --sblock=1"
./genozip $file --sblock 1 -ft -o ${output}.genozip || exit 1

test_count_genocat_lines() {
    cmd="./genocat ${output}.genozip $2"
    test_header "$cmd"
    ./genozip $1 -fo ${output}.genozip || exit 1
    wc=`$cmd | wc -l`
    if [[ $wc != $3 ]]; then
        echo "FAILED - expected $3 lines, but getting $wc"
        exit 1
    fi
}

# FASTA genocat tests
test_count_genocat_lines test-file.fa "--sequential" 9
test_count_genocat_lines test-file.fa "--header-only" 3
test_count_genocat_lines test-file.fa "--header-one" 3
test_count_genocat_lines test-file.fa "--no-header" 15
test_count_genocat_lines test-file.fa "--no-header --sequential" 6
test_count_genocat_lines test-file.fa "--grep cytochrome" 6
test_count_genocat_lines test-file.fa "--grep cytochrome --sequential " 2
test_count_genocat_lines test-file.fa "--grep cytochrome --sequential --no-header " 1

# FASTQ genocat tests
test_count_genocat_lines test-file.fq "--header-only" 2
test_count_genocat_lines test-file.fq "--header-one" 2
test_count_genocat_lines test-file.fq "--grep 8160" 4
test_count_genocat_lines test-file.fq "--grep 8160 --header-only" 1

files=`ls backward-compatibility-test/*.genozip` 
for file in $files; do
    test_header "$file - backward compatability test"

    if [ `basename $file .vcf.genozip` = test-file.1.1.3 ]; then # in v1 we didn't have the -t option
        ./genounzip ${file} -fo $output || exit 1
        cmp_2_files backward-compatibility-test/test-file.1.1.3.vcf $output
    else
        ./genounzip -t $file || exit 1
    fi
done

if `command -v gtshark >& /dev/null`; then
    test_header "test-file.vcf --gtshark"
    ./genozip test-file.vcf --gtshark -ft -o ${output}.genozip || exit 1
fi

test_header "test-file.vcf without FORMAT or samples"
cut -f1-8 test-file.vcf > test-input.vcf
./genozip test-input.vcf -ft -o ${output}.genozip || exit 1
rm test-input.vcf

if `command -v samtools >& /dev/null`; then
    test_header "test_file.sam - input and output as BAM"
    samtools view test-file.sam -OBAM -h > bam-test.input.bam    
    ./genozip bam-test.input.bam -fto ${output}.genozip || exit 1
    ./genounzip ${output}.genozip --force --output bam-test.output.bam
    cmp_2_files bam-test.input.bam bam-test.output.bam.fake-extension
    rm bam-test.input.bam bam-test.output.bam
fi

test_header "Testing subsets (~3 VBs) or real world files
"
rm -f test-data/*.genozip
./genozip -ft test-data/* || exit 1

printf "\nALL GOOD!\n"

rm -f $output ${output}.genozip
