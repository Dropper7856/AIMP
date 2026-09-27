namespace AIMP.Protocol;

public sealed class ProtocolException : Exception
{
    public ProtocolException(string message)
        : base(message)
    {
    }
}
