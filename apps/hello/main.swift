import UIKit

func sysctlString(_ name: String) -> String {
    var size = 0
    guard sysctlbyname(name, nil, &size, nil, 0) == 0, size > 0 else { return "?" }
    var buf = [CChar](repeating: 0, count: size)
    guard sysctlbyname(name, &buf, &size, nil, 0) == 0 else { return "?" }
    return String(cString: buf)
}

func resolve(_ host: String) -> String {
    var hints = addrinfo()
    hints.ai_family = AF_INET
    hints.ai_socktype = SOCK_STREAM
    var res: UnsafeMutablePointer<addrinfo>?
    let err = getaddrinfo(host, "80", &hints, &res)
    guard err == 0, let first = res else { return "error: \(String(cString: gai_strerror(err)))" }
    defer { freeaddrinfo(first) }
    var ips: [String] = []
    var p: UnsafeMutablePointer<addrinfo>? = first
    while let ai = p {
        var buf = [CChar](repeating: 0, count: Int(NI_MAXHOST))
        if getnameinfo(ai.pointee.ai_addr, ai.pointee.ai_addrlen, &buf, socklen_t(buf.count), nil, 0, NI_NUMERICHOST) == 0 {
            ips.append(String(cString: buf))
        }
        p = ai.pointee.ai_next
    }
    return ips.joined(separator: ", ")
}

final class HelloViewController: UIViewController {
    private let counter = UILabel()
    private let netResult = UILabel()
    private var taps = 0

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .systemIndigo

        let title = UILabel()
        title.text = "سلام از Redmi Note 8!"
        title.font = .boldSystemFont(ofSize: 30)

        let info = UILabel()
        info.numberOfLines = 0
        info.textAlignment = .center
        info.font = .systemFont(ofSize: 17)
        let device = UIDevice.current
        info.text = """
        Hello from ginkgo
        \(device.systemName) \(device.systemVersion)
        hw.machine: \(sysctlString("hw.machine"))
        kernel: \(sysctlString("kern.ostype")) \(sysctlString("kern.osrelease"))
        """

        let button = UIButton(type: .system)
        button.setTitle("Tap me", for: .normal)
        button.titleLabel?.font = .boldSystemFont(ofSize: 24)
        button.backgroundColor = .white
        button.setTitleColor(.systemIndigo, for: .normal)
        button.layer.cornerRadius = 14
        button.contentEdgeInsets = UIEdgeInsets(top: 12, left: 36, bottom: 12, right: 36)
        button.addTarget(self, action: #selector(tapped), for: .touchUpInside)

        counter.text = "taps: 0"
        counter.font = .monospacedDigitSystemFont(ofSize: 22, weight: .medium)

        let netButton = UIButton(type: .system)
        netButton.setTitle("Network test", for: .normal)
        netButton.titleLabel?.font = .boldSystemFont(ofSize: 22)
        netButton.backgroundColor = .white
        netButton.setTitleColor(.systemIndigo, for: .normal)
        netButton.layer.cornerRadius = 14
        netButton.contentEdgeInsets = UIEdgeInsets(top: 10, left: 28, bottom: 10, right: 28)
        netButton.addTarget(self, action: #selector(networkTest), for: .touchUpInside)

        netResult.numberOfLines = 0
        netResult.textAlignment = .left
        netResult.font = .monospacedSystemFont(ofSize: 14, weight: .regular)

        for label in [title, info, counter, netResult] {
            label.textColor = .white
        }
        let stack = UIStackView(arrangedSubviews: [title, info, button, counter, netButton, netResult])
        stack.axis = .vertical
        stack.alignment = .center
        stack.spacing = 28
        stack.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.centerXAnchor.constraint(equalTo: view.centerXAnchor),
            stack.centerYAnchor.constraint(equalTo: view.centerYAnchor),
            stack.leadingAnchor.constraint(greaterThanOrEqualTo: view.leadingAnchor, constant: 20),
        ])
    }

    @objc private func tapped() {
        taps += 1
        counter.text = "taps: \(taps)"
    }

    private func show(_ lines: [String]) {
        DispatchQueue.main.async { self.netResult.text = lines.joined(separator: "\n") }
    }

    @objc private func networkTest() {
        show(["testing..."])
        DispatchQueue.global().async {
            var lines = ["DNS example.com: \(resolve("example.com"))"]
            self.show(lines)
            let config = URLSessionConfiguration.ephemeral
            config.timeoutIntervalForRequest = 20
            let session = URLSession(configuration: config)
            for url in ["http://10.0.2.2:8000/", "http://example.com/", "https://www.apple.com/"] {
                let done = DispatchSemaphore(value: 0)
                var result = ""
                session.dataTask(with: URL(string: url)!) { data, response, error in
                    if let http = response as? HTTPURLResponse {
                        result = "HTTP \(http.statusCode), \(data?.count ?? 0) bytes"
                    } else {
                        result = "error: \(error?.localizedDescription ?? "?")"
                    }
                    done.signal()
                }.resume()
                done.wait()
                lines.append("\(url)\n  \(result)")
                self.show(lines)
            }
        }
    }
}

final class AppDelegate: UIResponder, UIApplicationDelegate {
    var window: UIWindow?

    func application(_ application: UIApplication,
                     didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?) -> Bool {
        let window = UIWindow(frame: UIScreen.main.bounds)
        window.rootViewController = HelloViewController()
        window.makeKeyAndVisible()
        self.window = window
        return true
    }
}

UIApplicationMain(CommandLine.argc, CommandLine.unsafeArgv, nil, NSStringFromClass(AppDelegate.self))
