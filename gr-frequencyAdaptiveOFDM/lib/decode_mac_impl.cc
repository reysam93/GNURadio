/* -*- c++ -*- */
/* 
 * Copyright 2016 Samuel Rey <samuel.rey.escudero@gmail.com>
 *                  Bastian Bloessl <bloessl@ccs-labs.org>
 * 
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "decode_mac_impl.h"

#include <gnuradio/io_signature.h>
#include <boost/crc.hpp>

#define LINKTYPE_IEEE802_11 105 /* http://www.tcpdump.org/linktypes.html */

namespace gr {
  namespace frequencyAdaptiveOFDM {

    decode_mac::sptr
    decode_mac::make(bool log, bool debug)
    {
      return gnuradio::get_initial_sptr
        (new decode_mac_impl(log, debug));
    }

    /*
     * The private constructor
     */
    decode_mac_impl::decode_mac_impl(bool log, bool debug):
     block("decode_mac",
              gr::io_signature::make(1, 1, 48),
              gr::io_signature::make(0, 0, 0)),
      d_log(log),
      d_debug(debug),
      d_snr(std::vector<double>(4,0)),
      d_nom_freq(0.0),
      d_freq_offset(0.0),
      d_ofdm(std::vector<int>(4, 0)),
      d_frame(d_ofdm, 0),
      d_frame_complete(true)
    {
      message_port_register_out(pmt::mp("out"));
    }


    decode_mac_impl::~decode_mac_impl()
    {
    }


    int
    decode_mac_impl::general_work (int noutput_items,
                       gr_vector_int &ninput_items,
                       gr_vector_const_void_star &input_items,
                       gr_vector_void_star &output_items)
    {
      const uint8_t *in = (const uint8_t*)input_items[0];

      int i = 0;

      std::vector<gr::tag_t> tags;
      const uint64_t nread = this->nitems_read(0);

      dout << "Decode MAC: input " << ninput_items[0] << std::endl;

      while(i < ninput_items[0]) {
        get_tags_in_range(tags, 0, nread + i, nread + i + 1,
                            pmt::string_to_symbol("wifi_start"));

        if(tags.size()) {
          if (d_frame_complete == false) {
            dout << "Warning: starting to receive new frame before old frame was complete" << std::endl;
            dout << "Already copied " << copied << " out of " << d_frame.n_sym << " symbols of last frame" << std::endl;
          }
          d_frame_complete = false;

          pmt::pmt_t dict = tags[0].value;
          int len_data = pmt::to_uint64(pmt::dict_ref(dict, pmt::mp("frame_bytes"), pmt::from_uint64(MAX_PSDU_SIZE+1)));
          std::vector<int> encoding = pmt::s32vector_elements(pmt::dict_ref(dict, pmt::mp("encoding"), pmt::init_s32vector(0, 0)));
          d_snr = pmt::f64vector_elements(pmt::dict_ref(dict, pmt::mp("snr"), pmt::init_f64vector(0,0)));
          d_nom_freq = pmt::to_double(pmt::dict_ref(dict, pmt::mp("freq"), pmt::from_double(0)));
          d_freq_offset = pmt::to_double(pmt::dict_ref(dict, pmt::mp("freq_offset"), pmt::from_double(0)));

          ofdm_param ofdm = ofdm_param(encoding);
          frame_param frame = frame_param(ofdm, len_data);

          // check for maximum frame size
          if(frame.n_sym <= MAX_SYM && frame.psdu_size <= MAX_PSDU_SIZE) {
            d_ofdm = ofdm;
            d_frame = frame;
            copied = 0;

            if (d_debug){
              dout << "Decode MAC: frame start -- len " << len_data << std::endl;
              d_frame.print();
              d_ofdm.print();
            }
          } else {
            dout << "Dropping frame which is too large (symbols or bits)" << std::endl;
          }
        }

        if(copied < d_frame.n_sym) {
          dout << "copy one symbol, copied " << copied << " out of " << d_frame.n_sym << std::endl;
          std::memcpy(d_rx_symbols + (copied * 48), in, 48);
          copied++;

          if(copied == d_frame.n_sym) {
            dout << "received complete frame - decoding" << std::endl;
            decode();
            in += 48;
            i++;
            d_frame_complete = true;
            break;
          }
        }

        in += 48;
        i++;
      }

      consume(0, i);
      return 0;
    }


    void 
    decode_mac_impl::decode(){
      regroup_symbols();      
      deinterleave();
      uint8_t *decoded = d_decoder.decode(&d_ofdm, &d_frame, d_deinterleaved_bits);
      descramble(decoded);
      print_output();

      // skip service field
      boost::crc_32_type result;
      result.process_bytes(out_bytes + 2, d_frame.psdu_size);
      if(result.checksum() != 558161692) {
        std::cerr << "\nERROR: DECODE MAC: checksum wrong -- dropping\n" << std::endl;
        return;
      }

      // create PDU
      pmt::pmt_t blob = pmt::make_blob(out_bytes + 2, d_frame.psdu_size - 4);
      pmt::pmt_t enc = pmt::init_s32vector(4, d_ofdm.resource_blocks_e);
      pmt::pmt_t dict = pmt::make_dict();
      dict = pmt::dict_add(dict, pmt::mp("encoding"), enc);
      dict = pmt::dict_add(dict, pmt::mp("snr"), pmt::init_f64vector(4, d_snr));
      dict = pmt::dict_add(dict, pmt::mp("nomfreq"), pmt::from_double(d_nom_freq));
      dict = pmt::dict_add(dict, pmt::mp("freqofs"), pmt::from_double(d_freq_offset));
      dict = pmt::dict_add(dict, pmt::mp("dlt"), pmt::from_long(LINKTYPE_IEEE802_11));
      message_port_pub(pmt::mp("out"), pmt::cons(dict, blob));
    }


    void
    decode_mac_impl::regroup_symbols(){
      int bpsc;
      int regrouped = 0;

      for(int i = 0; i < d_frame.n_sym * 48; i++) {
        if ((i % 48) < 12){
          bpsc = d_ofdm.n_bpcrb[0];
        }else if ((i % 48) < 24){
          bpsc = d_ofdm.n_bpcrb[1];
        }else if((i % 48) < 36){
          bpsc = d_ofdm.n_bpcrb[2];
        }else if((i % 48) < 48){
          bpsc = d_ofdm.n_bpcrb[3];
        }
        for(int k = 0; k < bpsc; k++) {
          d_rx_bits[regrouped] = !!(d_rx_symbols[i] & (1 << k));
          regrouped++;
        }
      }
    }


    void
    decode_mac_impl::deinterleave(){
      int n_cbps = d_ofdm.n_cbps;
      int first[n_cbps];
      int second[n_cbps];
      int s = std::max(int(d_ofdm.n_bpsc) / 2, 1);

      for(int j = 0; j < n_cbps; j++) {
        first[j] = s * (j / s) + ((j + int(floor(16.0 * j / n_cbps))) % s);
      }

      for(int i = 0; i < n_cbps; i++) {
        second[i] = 16 * i - (n_cbps - 1) * int(floor(16.0 * i / n_cbps));
      }

      int count = 0;
      for(int i = 0; i < d_frame.n_sym; i++) {
        for(int k = 0; k < n_cbps; k++) {
          d_deinterleaved_bits[i * n_cbps + second[first[k]]] = d_rx_bits[i * n_cbps + k];
        }
      }
    }


    void
    decode_mac_impl::descramble (uint8_t *decoded_bits){
      int state = 0;
      std::memset(out_bytes, 0, d_frame.psdu_size+2);

      for(int i = 0; i < 7; i++) {
        if(decoded_bits[i]) {
          state |= 1 << (6 - i);
        }
      }
      out_bytes[0] = state;

      int feedback;
      int bit;

      for(int i = 7; i < d_frame.psdu_size*8+16; i++) {
        feedback = ((!!(state & 64))) ^ (!!(state & 8));
        bit = feedback ^ (decoded_bits[i] & 0x1);
        out_bytes[i/8] |= bit << (i%8);
        state = ((state << 1) & 0x7e) | feedback;
      }
    }


    void
    decode_mac_impl::print_output(){
      dout << std::endl;
      dout << "psdu size: " << d_frame.psdu_size << std::endl;
      for(int i = 2; i < d_frame.psdu_size+2; i++) {
        dout << std::setfill('0') << std::setw(2) << std::hex << ((unsigned int)out_bytes[i] & 0xFF) << std::dec << " ";
        if(i % 16 == 15) {
          dout << std::endl;
        }
      }
      dout << std::endl;
      for(int i = 2; i < d_frame.psdu_size+2; i++) {
        if((out_bytes[i] > 31) && (out_bytes[i] < 127)) {
          dout << ((char) out_bytes[i]);
        } else {
          dout << ".";
        }
      }
      dout << std::endl;
    }

  } /* namespace frequencyAdaptiveOFDM */
} /* namespace gr */

