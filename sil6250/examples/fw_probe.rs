use sil6250::Engine;

fn main() {
    let mut engine = Engine::open("/dev/sil6250").expect("open sensor");

    match engine.firmware_version() {
        Ok(v) => {
            println!("Raw response: {:02x?}", v);
            println!("As decimal: {}.{}.{}.{}", v[0], v[1], v[2], v[3]);
            println!("As hex: {:02x?}", v);
        }
        Err(e) => {
            eprintln!("Firmware query failed: {e}");
            std::process::exit(1);
        }
    }
}
