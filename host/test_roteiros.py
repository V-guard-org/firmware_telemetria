"""Reading the roteiros (TESTES.md §3.2, seam 6 of spec #15)."""

import unittest

from simulator import RoteiroError, parse_roteiro


class ReadingRoteiros(unittest.TestCase):

    def test_times_comments_and_the_end(self):
        roteiro = parse_roteiro(
            "# Liga e reinicia\n"
            "\n"
            "0:00 ignição liga\n"
            "0:01 partida      # the engine turns\n"
            "1:15 fim\n")
        self.assertEqual([(0, 3, "ignição liga"), (1, 4, "partida")],
                         [(action.at_s, action.line, action.text) for action in roteiro.actions])
        self.assertEqual(75, roteiro.end_s)

    def test_an_unknown_command_stops_the_roteiro_and_names_its_line(self):
        with self.assertRaisesRegex(RoteiroError, r"^linha 3: .*acelerdor 30%"):
            parse_roteiro("0:00 ignição liga\n"
                          "# a typo below\n"
                          "0:04 acelerdor 30%\n"
                          "0:10 fim\n")

    def test_a_line_without_a_time_in_m_ss_names_its_line(self):
        for line in ("partida", "0:4 partida", "0:60 partida", "00 partida", "0:04"):
            with self.subTest(line=line), self.assertRaisesRegex(RoteiroError, r"^linha 2: "):
                parse_roteiro(f"0:00 ignição liga\n{line}\n")

    def test_without_fim_it_ends_5_s_after_the_last_action(self):
        self.assertEqual(12, parse_roteiro("0:00 ignição liga\n0:07 partida\n").end_s)

    def test_a_time_earlier_than_the_line_before_names_its_line(self):
        with self.assertRaisesRegex(RoteiroError, r"^linha 2: "):
            parse_roteiro("0:10 ignição liga\n0:05 partida\n")

    def test_an_action_after_fim_names_its_line(self):
        with self.assertRaisesRegex(RoteiroError, r"^linha 3: "):
            parse_roteiro("0:00 ignição liga\n0:10 fim\n0:12 partida\n")

    def test_the_commands_of_the_headless_simulator(self):
        commands = ["ignição liga", "ignição desliga", "partida",
                    "acelerador 0%", "acelerador 30%", "acelerador 100%", "freio 50%",
                    "volante -20%", "volante +100%", "volante 15%", "solta tudo",
                    "pi ping desliga", "pi ping liga", "reinicia esp32",
                    "confere imu ok", "confere imu falha", "confere boots 1",
                    "confere vazias obd 0", "confere vazias imu 12"]
        commands += [f"confere obd {state}" for state in (
            "esperando-pi", "escutando", "descobrindo", "lendo", "sem-resposta", "velocidade-errada",
            "linha-presa", "falha-can", "bloqueado")]
        roteiro = parse_roteiro("".join(f"0:01 {command}\n" for command in commands))
        self.assertEqual(commands, [action.text for action in roteiro.actions])

    def test_extra_spaces_between_words_are_fine(self):
        self.assertEqual("acelerador 30%", parse_roteiro("0:01   acelerador   30%  \n").actions[0].text)

    def test_a_value_out_of_range_names_its_line(self):
        for line in ("acelerador 130%", "acelerador -5%", "freio 101%", "volante -101%", "acelerador 30",
                     "confere obd voando", "confere imu bem", "confere boots -1", "confere vazias gps 1",
                     "pi ping talvez", "ignição"):
            with self.subTest(line=line), self.assertRaisesRegex(RoteiroError, r"^linha 2: "):
                parse_roteiro(f"0:00 partida\n0:01 {line}\n")

    def test_a_roteiro_with_no_action_does_not_run(self):
        for text in ("", "# só um comentário\n", "0:10 fim\n"):
            with self.subTest(text=text), self.assertRaises(RoteiroError):
                parse_roteiro(text)


if __name__ == "__main__":
    unittest.main()
