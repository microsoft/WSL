#
# Module to create CPIO packed strings.
#
# N.B. The library currently only supports minimal operations in the "New ASCII
#      Format" including:
#          +Adding files
#          +Adding trailer
#

package CPIOImage;

use strict;
use Math::BigInt qw/:constant/;
use Exporter 'import';
use File::Basename;
use bytes;
no bytes;

our @EXPORT = qw(AddObject,
                 AddTrailer,
                 WriteFile
                 );

#
# CPIO constants
#
use constant MAGIC_NEWC => "070701";
use constant HEADER_ENTRY_FORMAT => "%08X";
use constant CHECK => 0;
use constant NAME_EXTRA_PADDING => 2;
use constant HEADER_PADDING_SIZE => 4;
use constant FILE_PADDING_SIZE => 512;

use constant TRAILER => "TRAILER!!!";

#
# CPIO values assumed for now.
#
# N.B. These need to be dynamically generated if used outside of basic scenarios
#      like initramfs.
#
use constant INODE => 0;
use constant MODE => oct("100755");
use constant UID => 0;
use constant GID => 0;
use constant NLINK => 0;
use constant DEVMAJOR => 0;
use constant DEVMINOR => 0;
use constant RDEVMAJOR => 0;
use constant RDEVMINOR => 0;

sub ErrorExit($$) {
    my $line_number = shift;
    my $error_message = shift;

    print STDERR "$0($line_number, 1): error LX1: $error_message\n";
    exit 255;
}

#
# Routine Description:
#
#     This routine computes padding.
#
# Arguments:
#
#     size - Supplies the size to align.
#
#     alignment - Supplies the alignment
#
# Return Value:
#
#     The number of bytes to pad.
#
sub GetPadding($$) {
    my $Size = shift;
    my $Alignment = shift;

    my $Result = $Size % $Alignment;
    if ($Result != 0)
    {
        $Result = $Alignment - $Result;
    }

    return $Result;
}

#
# Routine Description:
#
#     This routine creates a CPIO entry.
#
# Arguments:
#
#     name - Supplies the name.
#
#     fileContentsRef - Supplies the file contents reference.
#
#     fileSize - Supplies the file size.
#
# Return Value:
#
#     None.
#
sub CreateEntry($$$) {
    my $name = shift;
    my $fileContentsRef = shift;
    my $fileSize = shift;

    # Write static values.
    my $cpioEntry = MAGIC_NEWC;
    $cpioEntry .= sprintf(HEADER_ENTRY_FORMAT, INODE);
    my $mode = sprintf(HEADER_ENTRY_FORMAT, MODE);
    $cpioEntry .= sprintf(HEADER_ENTRY_FORMAT, MODE);
    $cpioEntry .= sprintf(HEADER_ENTRY_FORMAT, UID);
    $cpioEntry .= sprintf(HEADER_ENTRY_FORMAT, GID);
    $cpioEntry .= sprintf(HEADER_ENTRY_FORMAT, NLINK);
    $cpioEntry .= sprintf(HEADER_ENTRY_FORMAT, time());

    # Write the file size
    $cpioEntry .= sprintf(HEADER_ENTRY_FORMAT,  $fileSize);

    # Write static values.
    $cpioEntry .= sprintf(HEADER_ENTRY_FORMAT,  DEVMAJOR);
    $cpioEntry .= sprintf(HEADER_ENTRY_FORMAT,  DEVMINOR);
    $cpioEntry .= sprintf(HEADER_ENTRY_FORMAT,  RDEVMAJOR);
    $cpioEntry .= sprintf(HEADER_ENTRY_FORMAT,  RDEVMINOR);

    # Write the name size including the NULL terminator.
    my $nameSize = length($name) + 1;
    $cpioEntry .= sprintf(HEADER_ENTRY_FORMAT,  $nameSize);

    # Write static value.
    $cpioEntry .= sprintf(HEADER_ENTRY_FORMAT,  CHECK);

    # Write the name including the NULL terminator, padded out to a multiple of 4
    $cpioEntry .= $name."\0";
    $cpioEntry .= "\0" x (GetPadding($nameSize + NAME_EXTRA_PADDING, HEADER_PADDING_SIZE));

    # Write the contents, padded out to a multiple of 4
    $cpioEntry .= ${$fileContentsRef};
    $cpioEntry .= "\0" x GetPadding($fileSize, HEADER_PADDING_SIZE);

    return $cpioEntry;
}

#
# Routine Description:
#
#     This routine adds an object to the cpio image stream.
#
#     N.B. Only files are currently supported.
#
# Arguments:
#
#     cpioImage - Supplies the cpio image.
#
#     path - Supplies the path of the object to add.
#
# Return Value:
#
#     None
#
sub AddObject($$$) {
    my $cpioImageRef = shift;
    my $path = shift;
    my $name = shift;

    # Open and read the file
    my $INPUT_HANDLE;
    unless(open($INPUT_HANDLE, "<", $path)){ErrorExit(__LINE__, $!);}
    binmode($INPUT_HANDLE);
    local $/ = undef;
    my $fileContents = <$INPUT_HANDLE>;
    close($INPUT_HANDLE);

    # Check the size of the file
    my $fileSize;
    unless(defined($fileSize = -s $path)){ErrorExit(__LINE__, "Could not check file size at line $.");}
    my $fileContentsSize = bytes::length($fileContents);
    if ($fileSize != $fileContentsSize){ErrorExit(__LINE__, "Invalid file size at line $.");}

    # Write the entry
    ${$cpioImageRef} .= CreateEntry($name, \$fileContents, $fileSize);
}

#
# Routine Description:
#
#     This routine adds the trailer to the cpio image stream.
#
# Arguments:
#
#     cpioImageRef - Supplies the cpio image reference.
#
# Return Value:
#
#     None
#
sub AddTrailer($) {
    my $cpioImageRef = shift;

    # Write the entry
    my $fileContents = "";
    ${$cpioImageRef} .= CreateEntry(TRAILER, \$fileContents, 0);

    # Pad out the file to the block size
    my $size = bytes::length(${$cpioImageRef});
    ${$cpioImageRef} .= "\0" x GetPadding($size, FILE_PADDING_SIZE);
}

#
# Routine Description:
#
#     This routine writes the cpio image stream to a file.
#
# Arguments:
#
#     cpioImageRef - Supplies the cpio image reference.
#
#     name - Supplies the name of the file.
#
# Return Value:
#
#     None
#
sub WriteFile($$) {
    my $cpioImageRef = shift;
    my $name = shift;

    my $OUTPUT_HANDLE;
    unless(open($OUTPUT_HANDLE, ">", $name)){ErrorExit(__LINE__, $!);}
    binmode($OUTPUT_HANDLE);
    print $OUTPUT_HANDLE ${$cpioImageRef};
    close($OUTPUT_HANDLE);
}