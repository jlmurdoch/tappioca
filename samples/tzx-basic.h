/*
 * A basic TZX example (a program that prints "HELLO")
 */

static uint8_t buf[] = {
	/*
	 * TZX Header (10 bytes)
	 */
	0x5a, 0x58, 0x54, 0x61, 0x70, 0x65, 0x21, 
	0x1a,		// End of Text
	0x01, 0x14, 	// Version
	/*
	 * Header Block - Standard (10)
	 */
	0x10, 		// ID (10 = Standard)
	0xe8, 0x03, 	// Delay after: 1000ms (0x03e8)
	0x13, 0x00, 	// Size: 19 (0x0013)
	// Payload starts here
	0x00, 		// Marker (Header)
	0x00, 0x48, 0x45, 0x4c, 0x4c, 0x4f, 0x20, 
	0x20, 0x20, 0x20, 0x20, 0x0d, 0x00, 0x00, 0x80, 
	0x0d, 0x00, 
	0xe2, 		// Checksum 
	/*
	 * Data Block - Standard (10)
	 */
	0x10, 		// ID (10 = Standard)
	0xe8, 0x03, 	// Delay after: 1000ms (0x03e8)
	0x0f, 0x00, 	// Size: 15 (0x000f)
	// Payload starts here
	0xff, 		// Marker (Data)
	0x00, 0x0a, 0x09, 0x00, 0xf5, 0x22, 0x48, 
	0x45, 0x4c, 0x4c, 0x4f, 0x22, 0x0d, 
	0x46 		// Checksum
};
