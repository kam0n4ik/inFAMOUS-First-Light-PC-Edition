# R9 stand routes (docs/R9_GAMEPLAY_PERFORMANCE.md §3): SecondSonPC's keyboard layout (docs/INPUT_MAPPING.md - W/A/S/D
# move, mouse camera, Space cross/jump, E circle/dash, V square/melee, R R1, Q L1, right mouse L2/aim, left mouse
# R2/fire), timed from the window's opening; mark:<phase> events split the run for the gameplay profiler. Each route
# starts the same: the title, Continue (Space at 12 s and 22 s), the world loaded by ~40 s.
#   Get-R9Route <name> -> Name, Press, Seconds, Window ("from:to" s for frame_stats), Shots (s, r9run -Shots route)
function Get-R9Route([string]$Name) {
    $title = @("space@12", "space@22")
    $ev = New-Object System.Collections.Generic.List[string]
    $title | ForEach-Object { $ev.Add($_) }
    switch ($Name) {
        "look" { # calibration: what the scene shows, how far a mouse move turns the camera, what a power does
            $ev.Add("w@60:2")
            $ev.Add("move:1000:0@66:0.3")
            $ev.Add("move:2000:0@70:0.3")
            $ev.Add("mouse_right@76:3"); $ev.Add("mouse_left@77:0.12"); $ev.Add("mouse_left@78:0.12")
            $ev.Add("r@82")
            $ev.Add("e@86:1.2"); $ev.Add("w@86:1.5")
            $ev.Add("v@90")
            $ev.Add("q@93")
            return [pscustomobject]@{ Name = "calibration"; Press = ($ev -join ","); Seconds = 100; Window = "40:100"
                Shots = "40,50,58,63,68,72,77.3,79,83,87,90.5,94,98" }
        }
        "calm" { # standing still in the world (the profile's reference)
            $ev.Add("mark:calm@45"); $ev.Add("mark:end@118")
            return [pscustomobject]@{ Name = "standing, no input"; Press = ($ev -join ","); Seconds = 120; Window = "45:118"; Shots = "50,80,115" }
        }
        "fight" { # powers fired around the start (aim + fire bursts, R1, melee, dash, camera turns)
            $ev.Add("mark:calm@45"); $ev.Add("mark:fight@60")
            for ($t = 60; $t -lt 136; $t += 8) {
                $ev.Add("mouse_right@$($t):4")
                foreach ($f in 0.3, 0.7, 1.1, 1.5, 1.9, 2.3, 2.7, 3.1, 3.5) { $ev.Add("mouse_left@$($t + $f):0.1") }
                $ev.Add("r@$($t + 4.4)")
                $ev.Add("v@$($t + 5.2)"); $ev.Add("v@$($t + 5.6)")
                $ev.Add("e@$($t + 6.2):0.6"); $ev.Add("w@$($t + 6.2):0.8")
                $ev.Add("move:900:0@$($t + 7.0):0.4")
            }
            $ev.Add("mark:end@140")
            return [pscustomobject]@{ Name = "powers fired around"; Press = ($ev -join ","); Seconds = 150; Window = "45:140"; Shots = "50,61,63,66,70,85,100,120,138" }
        }
        "traverse" { # running and dashing through the streets, camera curves and sharp turns
            $ev.Add("mark:calm@45"); $ev.Add("mark:traversal@55")
            $ev.Add("w@55:85")
            for ($t = 56; $t -lt 139; $t += 2) { $ev.Add("e@$($t):1.2") }
            for ($t = 57; $t -lt 139; $t += 3) { $ev.Add("space@$($t + 0.5)") }
            for ($t = 58; $t -lt 139; $t += 4) { $ev.Add("move:500:0@$($t):0.8") }
            for ($t = 65; $t -lt 139; $t += 15) { $ev.Add("move:2600:0@$($t):0.3") }
            $ev.Add("mark:end@140")
            return [pscustomobject]@{ Name = "run + dash + jumps + turns"; Press = ($ev -join ","); Seconds = 150; Window = "45:140"; Shots = "50,60,70,80,90,100,110,120,130,139" }
        }
        default { # a press spec of its own
            return [pscustomobject]@{ Name = "custom"; Press = $Name; Seconds = 150; Window = "45:148"; Shots = "" }
        }
    }
}
