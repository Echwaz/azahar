use libsave3ds::file_system::{FileSystem, FileSystemDir, FileSystemFile};
use libsave3ds::memory_file::MemoryFile;
use libsave3ds::save_data::{SaveData, SaveDataType};
use std::rc::Rc;

fn extract(input: &[u8]) -> Result<Vec<u8>, ()> {
    let save = SaveData::new(Rc::new(MemoryFile::new(input.to_vec())), SaveDataType::Bare)
        .map_err(|_| ())?;
    let root = save.open_dir(1).map_err(|_| ())?;
    let mut name = [0u8; 16];
    name[..12].copy_from_slice(b"Launcher.dat");
    let file = root.open_sub_file(name).map_err(|_| ())?;
    if file.len() != 0x2558 { return Err(()); }
    let mut result = vec![0u8; 0x2558];
    file.read(0, &mut result).map_err(|_| ())?;
    Ok(result)
}

#[no_mangle]
pub unsafe extern "C" fn artic_extract_home_layout(
    input: *const u8, length: usize, output: *mut u8, capacity: usize,
) -> i32 {
    if input.is_null() || output.is_null() || !(512..=1048576).contains(&length)
        || capacity != 0x2558 { return 1; }
    let result = std::panic::catch_unwind(|| {
        extract(std::slice::from_raw_parts(input, length))
    });
    match result {
        Ok(Ok(data)) => {
            std::ptr::copy_nonoverlapping(data.as_ptr(), output, data.len());
            0
        }
        Ok(Err(())) => 2,
        Err(_) => 3,
    }
}
