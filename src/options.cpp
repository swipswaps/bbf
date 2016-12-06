/*
  ISC License

  Copyright (c) 2016, Antonio SJ Musumeci <trapexit@spawn.link>

  Permission to use, copy, modify, and/or distribute this software for any
  purpose with or without fee is hereby granted, provided that the above
  copyright notice and this permission notice appear in all copies.

  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <iostream>

#include "errors.hpp"
#include "options.hpp"

#define BASE10 10

void
usage(std::ostream &os)
{
  os <<
    "usage: bbf [options] <instruction> <path>\n"
    "\n"
    "  instruction\n"
    "    info                  : print out details of the device\n"
    "    captcha               : print captcha for device\n"
    "    scan                  : perform scan for bad blocks by reading\n"
    "    fix                   : attempt to force drive to reallocate block\n"
    "                            * on successful read of block, write it back\n"
    "                            * on unsuccessful read of block, write zeros\n"
    "    burnin                : attempts a non-destructive write, read, & verify\n"
    "                            * read block, write block of 0x00, 0x55, 0xAA, 0xFF\n"
    "                            * write back original block if was successfully read\n"
    "                            * only if the last write,read,verify fails is it bad\n"
    "    find-files            : given a list of bad blocks try to find affected files\n"
    "    dump-files            : dump list of block ranges and files assocated with them\n"
    "    file-blocks           : dump a list of individual blocks a file uses\n"
    "    write-uncorrectable   : mark blocks as corrupted / uncorrectable\n"
    "  path                    : block device|directory|file to act on\n"
    "\n"
    "  -t, --rwtype <os|ata>   : use OS or ATA reads and writes (default: os)\n"
    "  -q, --quiet             : redirects stdout to /dev/null\n"
    "  -s, --start-block <lba> : block to start from (default: 0)\n"
    "  -e, --end-block <lba>   : block to stop at (default: last block)\n"
    "  -o, --output <file>     : file to write bad block list to\n"
    "  -i, --input <file>      : file to read bad block list from\n"
    "  -r, --retries <count>   : number of retries on certain reads & writes\n"
    "  -c, --captcha <captcha> : needed when performing destructive operations\n"
    "\n";
}

AppError
Options::process_arg(const int          argc,
                     const char * const argv[],
                     const int          opt)
{
  switch(opt)
    {
    case 'q':
      quiet++;
      break;
    case 'r':
      errno = 0;
      retries = ::strtol(optarg,NULL,BASE10);
      if(((errno == ERANGE) && (retries == LONG_MAX)) || (retries < 1))
       return AppError::argument_invalid("retries invalid");
      break;
    case 's':
      errno = 0;
      start_block = ::strtoull(optarg,NULL,BASE10);
      if((start_block == ULLONG_MAX) && (errno == ERANGE))
        return AppError::argument_invalid("start block value is invalid");
      break;
    case 'e':
      errno = 0;
      end_block = ::strtoull(optarg,NULL,BASE10);
      if((end_block == ULLONG_MAX) && (errno == ERANGE))
        return AppError::argument_invalid("end block value is invalid");
      break;
    case 'o':
      output_file = optarg;
      break;
    case 'i':
      input_file = optarg;
      break;
    case 'c':
      captcha = optarg;
      break;
    case 't':
      if(!strcmp(optarg,"os"))
        rwtype = OS;
      else if(!strcmp(optarg,"ata"))
        rwtype = ATA;
      else
        return AppError::argument_invalid("valid rwtype values are 'os' or 'ata'");
      break;
    case 'h':
      usage(std::cout);
      return AppError::success();
    case '?':
      return AppError::argument_invalid(std::string(argv[optind-1]) + " is unknown");
    }

  return AppError::success();
}

Options::Instruction
Options::instr_from_string(const std::string str)
{
  if(str == "info")
    return Options::INFO;
  if(str == "captcha")
    return Options::CAPTCHA;
  if(str == "scan")
    return Options::SCAN;
  if(str == "fix")
    return Options::FIX;
  if(str == "burnin")
    return Options::BURNIN;
  if(str == "find-files")
    return Options::FIND_FILES;
  if(str == "dump-files")
    return Options::DUMP_FILES;
  if(str == "file-blocks")
    return Options::FILE_BLOCKS;
  if(str == "write-uncorrectable")
    return Options::WRITE_UNCORRECTABLE;

  return Options::_INVALID;
}

AppError
Options::parse(const int argc,
               char * const argv[])
{
  static const char short_options[] = "hqt:r:s:e:o:i:c:";
  static const struct option long_options[] =
    {
      {"help",              no_argument, NULL, 'h'},
      {"quiet",             no_argument, NULL, 'q'},
      {"rwtype",      required_argument, NULL, 't'},
      {"retries",     required_argument, NULL, 'r'},
      {"start-block", required_argument, NULL, 's'},
      {"end-block",   required_argument, NULL, 'e'},
      {"output",      required_argument, NULL, 'o'},
      {"input",       required_argument, NULL, 'i'},
      {"captcha",     required_argument, NULL, 'c'},
      {NULL,                          0, NULL,   0}
    };

  if(argc == 1)
    {
      usage(std::cout);
      return AppError::success();
    }

  opterr = 0;
  while(true)
    {
      int rv;
      AppError error;

      rv = getopt_long(argc,argv,short_options,long_options,NULL);
      if(rv == -1)
        break;

      error = process_arg(argc,argv,rv);
      if(!error.succeeded())
        return error;
    }

  if(argc == optind)
    return AppError::argument_required("instruction");
  if(argc == (optind + 1))
    return AppError::argument_required("target");

  instruction = instr_from_string(argv[optind]);
  device      = argv[optind+1];

  return validate();
}

AppError
Options::validate(void) const
{
  switch(instruction)
    {
    case Options::_INVALID:
      return AppError::argument_invalid("instruction is invalid");
    case Options::BURNIN:
      if(captcha.empty())
        return AppError::argument_required("captcha");
    case Options::SCAN:
      if(output_file.empty())
        return AppError::argument_required("bad block output file");
      break;
    case Options::FIX:
    case Options::WRITE_UNCORRECTABLE:
      if(captcha.empty())
        return AppError::argument_required("captcha");
    case Options::FIND_FILES:
      if(input_file.empty())
        return AppError::argument_required("bad block input file");
      break;
    case Options::DUMP_FILES:
    case Options::INFO:
    case Options::CAPTCHA:
      break;
    default:
      break;
    }

  if(start_block >= end_block)
    return AppError::argument_invalid("start block >= end block");

  return AppError::success();
}