extern crate clap;
extern crate portaudio;
extern crate crossbeam;
extern crate opus;

use std::{io, io::{Write, Read}, env, thread, result::Result,
          mem::size_of, sync::Arc};
use clap::{Arg, App, SubCommand};
use portaudio::PortAudio;
use byteorder::{LittleEndian, WriteBytesExt, ReadBytesExt};
use crossbeam::queue::ArrayQueue;

// Audio Parameters
type Sample = i16;
const SAMPLERATE:    u32   = 48000;
const CHANNELS:      u32   = 2;
const FRAME_MS:      u32   = 20;
const FRAME_SAMPLES: usize = ((FRAME_MS*SAMPLERATE)/1000) as usize;
const FRAME_LEN:     usize = FRAME_SAMPLES * CHANNELS as usize;
const FRAME_BYTES:   usize = size_of::<Sample>() * FRAME_LEN;
type Frame = [Sample; FRAME_LEN];

// Opus Parameters
const OPUS_BITRATE: opus::Bitrate = opus::Bitrate::Bits(96000);
const OPUS_VBR:         bool      = true;
const OPUS_FEC:         bool      = false;
const OPUS_PACKET_LOSS: i32       = 5; // in percent

fn opus_channels() -> opus::Channels {
    match CHANNELS {
        1 => opus::Channels::Mono,
        2 => opus::Channels::Stereo,
        _ => panic!("Invalid number `{}` of channels!", CHANNELS)
    }
}

#[derive(Debug)]
enum SndlinkError {
    PortAudio(portaudio::Error),
    Clap(clap::Error),
    Io(io::Error),
    Opus(opus::Error),
}

impl From<portaudio::Error> for SndlinkError {
    fn from(err: portaudio::Error) -> SndlinkError {
        SndlinkError::PortAudio(err)
    }
}

impl From<clap::Error> for SndlinkError {
    fn from(err: clap::Error) -> SndlinkError {
        SndlinkError::Clap(err)
    }
}

impl From<io::Error> for SndlinkError {
    fn from(err: io::Error) -> SndlinkError {
        SndlinkError::Io(err)
    }
}

impl From<opus::Error> for SndlinkError {
    fn from(err: opus::Error) -> SndlinkError {
        SndlinkError::Opus(err)
    }
}


fn main() -> Result<(), SndlinkError> {
    let mut argspec = App::new("sndlink")
        .version("0.1.0")
        .about("Transmit audio streams over the network")
        .author("Karolin Varner <karo@cupdev.net>")
        .subcommand(SubCommand::with_name("list-devs")
            .about("List available audio devices"))
        .subcommand(SubCommand::with_name("client")
            .about("Send audio to a server")
            .arg(Arg::with_name("SERVER").required(true))
            .arg(Arg::with_name("PORT").required(true))
            .arg(Arg::with_name("dev")
                .help("Select the audio device to use")
                .short("d")
                .long("dev")))
        .subcommand(SubCommand::with_name("server")
            .about("Receive audio from a server")
            .arg(Arg::with_name("PORT").required(true))
            .arg(Arg::with_name("dev")
                .help("Select the audio device to use")
                .short("d")
                .long("dev")));

    let argv = argspec
        .get_matches_from_safe_borrow(&mut env::args_os())
        .unwrap_or_else( |e| e.exit() );

    match argv.subcommand_name() {
        Some("list-devs")  => {
            list_devs()?;
        },
        Some("client")   => {
            client()?;
        },
        Some("server") => {
            server()?;
        },
        _              => {
            argspec.print_long_help();
        }
    }

    Ok(())
}

fn list_devs() -> Result<(), SndlinkError> {
    let pa = PortAudio::new()?;

    for dev in pa.devices()? {
        let (portaudio::DeviceIndex(idx), info) = dev?;
        println!("    {} {}", idx, info.name);
    }

    Ok(())
}

fn client() -> Result<(), SndlinkError> {
    let pa = PortAudio::new()?;

    let def_input = pa.default_input_device()?;
	let input_info = pa.device_info(def_input)?;

    let mut stream = pa.open_blocking_stream(
        portaudio::InputStreamSettings::new(
                portaudio::StreamParameters::<Sample>::new(
                    def_input,
                    CHANNELS as i32, // channels
                    true, // interleaved
                    input_info.default_low_input_latency),
                SAMPLERATE as f64, // sample rate
                FRAME_SAMPLES as u32))?;

    let mut encoder = opus::Encoder::new(
        SAMPLERATE as u32,
        opus_channels(),
        opus::Application::Voip)?;
    encoder.set_bitrate(OPUS_BITRATE)?;
    encoder.set_vbr(OPUS_VBR)?;
    encoder.set_inband_fec(OPUS_FEC)?;
    encoder.set_packet_loss_perc(OPUS_PACKET_LOSS)?;

    stream.start()?;

    let mut encoded : [u8; FRAME_BYTES] = [0; FRAME_BYTES];
    loop {
        let pcm = stream.read(FRAME_SAMPLES as u32)?;
        let len = encoder.encode(&pcm, &mut encoded)?;
        io::stdout().write_i16::<LittleEndian>(len as i16)?;
        io::stdout().write_all(&encoded[0..len])?;
    }
}


fn server() -> Result<(), SndlinkError> {
    let pa = PortAudio::new()?;

    let def_output = pa.default_output_device()?;
	let output_info = pa.device_info(def_output)?;

    let mut stream = pa.open_blocking_stream(
        portaudio::OutputStreamSettings::new(
            portaudio::StreamParameters::<Sample>::new(
                def_output,
                CHANNELS as i32, // channels
                true, // interleaved
                output_info.default_low_output_latency),
            SAMPLERATE as f64, // sample rate
            FRAME_SAMPLES as u32))?;

    let mut decoder = opus::Decoder::new(
        SAMPLERATE as u32,
        opus_channels())?;

    let iq = Arc::new(ArrayQueue::<Frame>::new(20));
    let oq = iq.clone();

    stream.start()?;

    thread::spawn(move || -> Result<(), SndlinkError> {
        let mut encoded : [u8; 10240] = [0; 10240];
        let mut pcm : Frame = [0; FRAME_LEN];
        loop {
            let len = io::stdin().read_i16::<LittleEndian>()? as usize;
            io::stdin().read_exact(&mut encoded[0..len])?;
            decoder.decode(&encoded[0..len], &mut pcm, OPUS_FEC)?;
            iq.push(pcm);
        }
    });


    loop {
        stream.write(FRAME_SAMPLES as u32, |buf| {
            match oq.pop() {
                Ok(from) => {
                    buf.copy_from_slice(&from);
                },
                _ => {
                    for mut v in buf {
                        *v = 0;
                    }
                }
            }
        });
    }
}
