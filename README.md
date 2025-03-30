# RFID Tool for Flipper Zero

A simple and powerful RFID tool for the Flipper Zero that allows reading, writing, and emulating RFID tags with data modification capabilities.

## Features

- Read RFID tags (125kHz)
- Write modified data back to RFID tags
- Data modification with configurable offset

- Future: Emulate RFID tags


## Usage

1. Launch the application
2. Main menu options:
   - Press `OK` to read a tag
   - Press `Up` to write modified data to a tag
   - Press `Back` to exit
3. When reading:
   - Place the RFID tag near the Flipper Zero
   - The device will read the tag and add an offset of 1 to each byte
   - You'll see a success notification if the tag is read
4. When writing:
   - Make sure you have previously read a tag
   - Place the target RFID tag near the Flipper Zero
   - The modified data will be written to the tag
   - You'll get a success/error notification

## Technical Details

- Uses the Flipper Zero's built-in RFID hardware
- Supports 125kHz RFID tags
- Data modification: Adds a constant offset

## Building

### Using uFBT (Recommended)

1. Install uFBT:
   ```bash
   pip install ufbt
   ```

2. Make sure you're in the correct directory structure:
   ```
   rfid_app/
   ├── application.fam
   ├── rfid_app.c
   ├── images/
   └── README.md
   ```

3. Build the application:
   ```bash
   ufbt build APPID=rfid_app
   ```

4. Install to Flipper Zero:
Supposed to be able to:
   ```bash
   ufbt flash APPID=rfid_app
   ```
Been manually copying with qFlipper

## Safety Notes (general)

- Only use this tool on RFID tags you own or have permission to modify
- Be careful when writing to tags as it may permanently modify them
- Some RFID tags may be read-only and cannot be written to

## License

This project is licensed under the MIT License - see the LICENSE file for details.

## Contributing

Feel free to submit issues and enhancement requests!