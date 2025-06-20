#
# Script to convert a binary to a CPIO image.
#

use strict;
use File::Basename;
use lib dirname (__FILE__);
use CPIOImage;

if($#ARGV + 1 != 2)
{
    ErrorExit(__LINE__, "Invalid command line at line $.");
}

my $inputFile = $ARGV[0];
my $outputFile = $ARGV[1];

my $cpioStream = "";
CPIOImage::AddObject(\$cpioStream, $inputFile, "init");
CPIOImage::AddTrailer(\$cpioStream);
CPIOImage::WriteFile(\$cpioStream, $outputFile);
exit 0;

sub ErrorExit($$) {
    my $line_number = shift;
    my $error_message = shift;

    print STDERR "$0($line_number, 1): error LX2: $error_message\n";
    exit 255;
}