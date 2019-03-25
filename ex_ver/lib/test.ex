#
#
# t = Task.start fn()->
#   res = "asdadadadadasdadasdasdada"
#   {:ok, asock} = :gen_udp.open 0, [mode: :binary]
#   Enum.each 1..10000, fn(x)->
#     :gen_udp.send asock, '100.67.61.151', 7070, res
#     :timer.sleep 1
#   end
# end
