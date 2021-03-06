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
#include <stdint.h>

#include <iostream>
#include <utility>

#include "badblockfile.hpp"
#include "blkdev.hpp"
#include "captcha.hpp"
#include "errors.hpp"
#include "info.hpp"
#include "math.hpp"
#include "options.hpp"
#include "signals.hpp"
#include "time.hpp"

static
uint64_t
trim_stepping(const BlkDev   &blkdev_,
              const uint64_t  block_,
              const uint64_t  stepping_)
{
  uint64_t block_count;

  block_count = blkdev_.logical_block_count();

  if(block_ > block_count)
    return 0;

  return std::min(block_count - block_,stepping_);
}

static
int
write_read_compare(BlkDev         &blkdev,
                   const uint64_t  stepping_,
                   const uint64_t  block,
                   char           *buf_,
                   const size_t    buflen_,
                   const int       retries,
                   const char     *write_buf)
{
  int rv;

  rv = -1;
  for(uint64_t i = 0; ((i <= retries) && (rv < 0)); i++)
    rv = blkdev.write(block,stepping_,write_buf,buflen_);

  if(rv < 0)
    return rv;

  rv = -1;
  for(uint64_t i = 0; ((i <= retries) && (rv < 0)); i++)
    rv = blkdev.read(block,stepping_,buf_,buflen_);

  if(rv < 0)
    return rv;

  rv = ::memcmp(write_buf,buf_,buflen_);
  if(rv != 0)
    return -EIO;

  return 0;
}

static
int
burn_block(BlkDev         &blkdev,
           const uint64_t  stepping_,
           const uint64_t  block,
           char           *buf_,
           const size_t    buflen_,
           const uint64_t  retries,
           const std::vector<std::vector<char> > &patterns_)
{
  int rv;

  rv = -1;
  for(uint64_t i = 0; ((i <= retries) && (rv < 0)); i++)
    rv = blkdev.read(block,stepping_,buf_,buflen_);

  if(rv < 0)
    ::memset(buf_,0,buflen_);

  for(uint64_t i = 0; i < patterns_.size(); i++)
    rv = write_read_compare(blkdev,stepping_,block,buf_,buflen_,retries,&patterns_[i][0]);

  rv = -1;
  for(uint64_t i = 0; ((i <= retries) && (rv < 0)); i++)
    rv = blkdev.write(block,stepping_,buf_,buflen_);

  return rv;
}

static
int
burnin_loop(BlkDev                &blkdev,
            const uint64_t         start_block,
            const uint64_t         end_block,
            const uint64_t         stepping_,
            char                  *buf_,
            const size_t           buflen_,
            std::vector<uint64_t> &badblocks,
            const uint64_t         max_errors_,
            const int              retries)
{
  int rv;
  uint64_t block;
  uint64_t stepping;
  double current_time;
  std::vector<std::vector<char> > patterns;
  const double start_time = Time::get_monotonic();

  patterns.resize(4);
  patterns[0].resize(buflen_,0x00);
  patterns[1].resize(buflen_,0x55);
  patterns[2].resize(buflen_,0xAA);
  patterns[3].resize(buflen_,0xFF);

  current_time = Time::get_monotonic();
  Info::print(std::cout,start_time,current_time,
              start_block,end_block,start_block,badblocks);

  block = start_block;
  while(block < end_block)
    {
      if(signals::signaled_to_exit())
        break;

      if(signals::dec(SIGALRM))
        {
          signals::alarm(1);
          current_time = Time::get_monotonic();
          Info::print(std::cout,start_time,current_time,
                      start_block,end_block,block,badblocks);
        }

      stepping = trim_stepping(blkdev,block,stepping_);

      rv = burn_block(blkdev,stepping_,block,buf_,buflen_,retries,patterns);
      block += stepping;
      if(rv >= 0)
        continue;
      if(rv == -EINVAL)
        break;

      current_time = Time::get_monotonic();
      Info::print(std::cout,start_time,current_time,
                  start_block,end_block,block,badblocks);

      for(uint64_t i = 0; i < stepping; i++)
        badblocks.push_back(block+i);

      if(badblocks.size() > max_errors_)
        break;
    }

  current_time = Time::get_monotonic();
  Info::print(std::cout,start_time,current_time,
              start_block,end_block,block,badblocks);

  return rv;
}

static
AppError
burnin(BlkDev                &blkdev,
       const Options         &opts,
       std::vector<uint64_t> &badblocks)
{
  int       rv;
  int       retries;
  char     *buf;
  size_t    buflen;
  uint64_t  start_block;
  uint64_t  end_block;
  uint64_t  stepping;

  retries     = opts.retries;
  stepping    = ((opts.stepping == 0) ?
                 blkdev.block_stepping() :
                 opts.stepping);
  buflen      = (stepping * blkdev.logical_block_size());
  start_block = math::round_down(opts.start_block,stepping);
  end_block   = std::min(opts.end_block,blkdev.logical_block_count());
  end_block   = math::round_up(end_block,stepping);
  end_block   = std::min(end_block,blkdev.logical_block_count());

  std::cout << "start block: "
            << start_block << std::endl
            << "end block: "
            << end_block << std::endl
            << "stepping: "
            << stepping << std::endl
            << "logical block size: "
            << blkdev.logical_block_size() << std::endl
            << "physical block size: "
            << blkdev.physical_block_size() << std::endl
            << "r/w size: "
            << stepping << " blocks / "
            << buflen << " bytes"
            << std::endl;

  signals::alarm(1);

  std::cout << "\r\x1B[2KBurning: "
            << start_block
            << " - "
            << end_block
            << std::endl;

  buf = new char[buflen];
  rv = burnin_loop(blkdev,
                   start_block,
                   end_block,
                   stepping,
                   buf,
                   buflen,
                   badblocks,
                   opts.max_errors,
                   retries);
  delete[] buf;

  std::cout << std::endl;

  if(rv < 0)
    return AppError::runtime(-rv,"error when performing burnin");

  return AppError::success();
}

static
void
set_blkdev_rwtype(BlkDev                &blkdev,
                  const Options::RWType  rwtype)
{
  switch(rwtype)
    {
    case Options::ATA:
      blkdev.set_rw_ata();
      break;
    case Options::OS:
      blkdev.set_rw_os();
      break;
    }
}

static
AppError
burnin(const Options &opts)
{
  int rv;
  AppError err;
  BlkDev blkdev;
  std::string captcha;
  std::string input_file;
  std::string output_file;
  std::vector<uint64_t> badblocks;

  input_file  = opts.input_file;
  output_file = opts.output_file;

  rv = blkdev.open_rdwr(opts.device,!opts.force);
  if(rv < 0)
    return AppError::opening_device(-rv,opts.device);

  captcha = captcha::calculate(blkdev);
  if(opts.captcha != captcha)
    return AppError::captcha(opts.captcha,captcha);

  if(output_file.empty())
    output_file = BadBlockFile::filepath(blkdev);

  if(input_file.empty())
    input_file = output_file;

  rv = BadBlockFile::read(input_file,badblocks);
  if(rv < 0)
    std::cout << "Warning: unable to open " << input_file << std::endl;
  else
    std::cout << "Imported bad blocks from " << input_file << std::endl;

  set_blkdev_rwtype(blkdev,opts.rwtype);

  err = burnin(blkdev,opts,badblocks);

  rv = BadBlockFile::write(output_file,badblocks);
  if((rv < 0) && err.succeeded())
    err = AppError::writing_badblocks_file(-rv,output_file);
  else if(!badblocks.empty())
    std::cout << "Bad blocks written to " << output_file << std::endl;

  rv = blkdev.close();
  if((rv < 0) && err.succeeded())
    err = AppError::closing_device(-rv,opts.device);

  return err;
}

namespace bbf
{
  AppError
  burnin(const Options &opts)
  {
    return ::burnin(opts);
  }
}
