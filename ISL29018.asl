// ADXL345 accelerometer device node
Device (ALSD)
{
	Name (_HID, "ISL29023")
	Name (_DDN, "Intersil 29023 Ambient Light Sensor")
	Name (_UID, 6)
	Name (_CRS, ResourceTemplate()
	{

		I2cSerialBus (
			0x44,                     // SlaveAddress
			ControllerInitiated,      // SlaveMode
			400000,                   // ConnectionSpeed
			AddressingMode7Bit,       // AddressingMode
			"\\_SB.PCI0.I2C1"         // ResourceSource
		)

		Interrupt (ResourceConsumer, Edge, ActiveLow)
		{
			BOARD_LIGHTSENSOR_IRQ
		}
	})

	Method (_STA)
	{
		If (LEqual (\S2EN, 1)) {
			Return (0xF)
		} Else {
			Return (0x0)
		}
	}
}
