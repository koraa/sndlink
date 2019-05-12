extern crate clap;
extern crate portaudio;
extern crate crossbeam;

use std::{io, io::Write, env, thread, result::Result,
          ptr::copy_nonoverlapping, sync::Arc};
use clap::{Arg, App, SubCommand};
use portaudio::PortAudio;
use byteorder::{LittleEndian, WriteBytesExt, ReadBytesExt};
use crossbeam::queue::ArrayQueue;

#[derive(Debug)]
enum SndlinkError {
    PortAudio(portaudio::Error),
    Clap(clap::Error),
    Io(io::Error)
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
    let mut pa = PortAudio::new()?;

    for dev in pa.devices()? {
        let (portaudio::DeviceIndex(idx), info) = dev?;
        println!("    {} {}", idx, info.name);
    }

    Ok(())
}

fn client() -> Result<(), SndlinkError> {
    let mut pa = PortAudio::new()?;

    let def_input = pa.default_input_device()?;
	let input_info = pa.device_info(def_input)?;

    let settings = portaudio::InputStreamSettings::new(
        portaudio::StreamParameters::<i16>::new(
            def_input,
            2, // channels
            true, // interleaved
            input_info.default_low_input_latency),
        48000.0, // sample rate
        2400);

    let mut stream = pa.open_blocking_stream(settings)?;
    stream.start();

    loop {
        for val in stream.read(2400)? {
            io::stdout().write_i16::<LittleEndian>(*val);
        }
    }
}


fn server() -> Result<(), SndlinkError> {
    let mut pa = PortAudio::new()?;

    let def_output = pa.default_output_device()?;
	let output_info = pa.device_info(def_output)?;

    let settings = portaudio::OutputStreamSettings::new(
        portaudio::StreamParameters::<i16>::new(
            def_output,
            2, // channels
            true, // interleaved
            output_info.default_low_output_latency),
        48000.0, // sample rate
        2400);

    let mut stream = pa.open_blocking_stream(settings)?;
    stream.start();

    let iq = Arc::new(ArrayQueue::<[i16; 4800]>::new(20));
    let oq = iq.clone();

    thread::spawn(move || -> Result<(), SndlinkError> {
        loop {
            let mut buf : [i16; 4800] = [0; 4800];
            io::stdin().read_i16_into::<LittleEndian>(&mut buf)?;
            iq.push(buf);
        }
    });


    loop {
        stream.write(2400, |mut buf| {
            match oq.pop() {
                Ok(from) => {
                    buf.copy_from_slice(&from);
                },
                _ => {
                }
            }
        });
    }
}
