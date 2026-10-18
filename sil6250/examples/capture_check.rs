use sil6250::{Engine, Frame, PskMode, IMG_SIZE};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut engine = Engine::open("/dev/sil6250")?;
    engine.set_psk_mode(PskMode::Auto);

    println!("Приложи палец к датчику в течение 15 секунд...");
    let raw = engine.capture_frame(15_000)?;
    let frame = Frame::from_raw(&raw);

    println!("Захват: OK");
    println!("Размер кадра: {} байт (ожидается {})", raw.len(), IMG_SIZE);
    println!("Качество кадра: {:.3}", frame.quality());
    Ok(())
}
