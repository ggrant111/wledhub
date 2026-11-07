import pathlib, re, unittest
h = (pathlib.Path(__file__).resolve().parents[1]/'include/ddp_packet.h').read_text()
class PacketTests(unittest.TestCase):
 def test_ddp_port(self): self.assertIn('PORT = 4048', h)
 def test_payload_limit(self): self.assertIn('PIXELS_PER_PACKET = 480', h)
 def test_10_byte_header(self): self.assertIn('HEADER_LEN = 10', h)
 def test_push_last_chunk(self): self.assertIn('push ? 0x01', h)
 def test_type_rgb24(self): self.assertIn('0x0B', h)
 def test_offsets(self):
  for shift in ['24','16','8']:
   self.assertIn('offsetBytes >> '+shift,h)
 def test_sequence(self): self.assertIn('sequence & 0x0F',h)
if __name__ == '__main__': unittest.main()
